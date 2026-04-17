#include "dbus_environment.hpp"

#include "helpers.hpp"

#include <systemd/sd-bus.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <map>
#include <string>
#include <thread>

namespace
{

int loggingCreateHandler(sd_bus_message* msg, void*, sd_bus_error*)
{
    return sd_bus_reply_method_return(msg, nullptr);
}

// Accepts: string message, string severity, dict{string,string} additionalData
const sd_bus_vtable loggingVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Create", "ssa{ss}", "", loggingCreateHandler,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

} // namespace

DbusEnvironment::~DbusEnvironment()
{
    if (setUp == true)
    {
        setUp = false;

        loggingStop = true;
        if (loggingThread.joinable())
        {
            loggingThread.join();
        }

        objServer = nullptr;
        bus = nullptr;
    }
}

void DbusEnvironment::SetUp()
{
    if (setUp == false)
    {
        setUp = true;

        bus = std::make_shared<sdbusplus::asio::connection>(ioc);
        bus->request_name(serviceName());

        objServer = std::make_unique<sdbusplus::asio::object_server>(bus);

        // Always redirect new_default_system() to the test D-Bus daemon so
        // that trigger_actions' synchronous D-Bus calls reach our mock service.
        // Use overwrite=1 so any pre-existing DBUS_SYSTEM_BUS_ADDRESS (which
        // would point to the real system bus) is replaced.
        if (const char* sessionAddr = std::getenv("DBUS_SESSION_BUS_ADDRESS"))
        {
            ::setenv("DBUS_SYSTEM_BUS_ADDRESS", sessionAddr, 1);
        }

        // Spin up a mock xyz.openbmc_project.Logging service using the raw
        // sd_bus C API in a dedicated std::thread.  This avoids boost::asio
        // entirely, sidestepping the BOOST_ASIO_DISABLE_THREADS constraint
        // that forbids sharing an io_context across threads.
        //
        // A promise/future pair ensures SetUp() blocks until the service name
        // is successfully registered before any test can run.
        std::promise<void> serviceReady;
        auto serviceReadyFuture = serviceReady.get_future();

        loggingStop = false;
        loggingThread = std::thread(
            [ready = std::move(serviceReady)](auto stop) mutable {
                sd_bus* logBus = nullptr;
                if (sd_bus_open_system(&logBus) < 0)
                {
                    ready.set_value();
                    return;
                }
                if (sd_bus_request_name(logBus, "xyz.openbmc_project.Logging",
                                        0) < 0)
                {
                    sd_bus_unref(logBus);
                    ready.set_value();
                    return;
                }
                sd_bus_slot* slot = nullptr;
                sd_bus_add_object_vtable(logBus, &slot,
                                         "/xyz/openbmc_project/logging",
                                         "xyz.openbmc_project.Logging.Create",
                                         loggingVtable, nullptr);

                ready.set_value(); // service is registered, tests may proceed

                while (!stop.get())
                {
                    sd_bus_process(logBus, nullptr);
                    sd_bus_wait(logBus, 10'000 /* µs = 10 ms */);
                }

                sd_bus_slot_unref(slot);
                sd_bus_flush_close_unref(logBus);
            },
            std::ref(loggingStop));

        serviceReadyFuture.wait(); // block until name is registered
    }
}

void DbusEnvironment::TearDown()
{
    ioc.poll();

    futures.clear();
}

boost::asio::io_context& DbusEnvironment::getIoc()
{
    return ioc;
}

std::shared_ptr<sdbusplus::asio::connection> DbusEnvironment::getBus()
{
    return bus;
}

std::shared_ptr<sdbusplus::asio::object_server> DbusEnvironment::getObjServer()
{
    return objServer;
}

const char* DbusEnvironment::serviceName()
{
    return "telemetry.ut";
}

std::function<void()> DbusEnvironment::setPromise(std::string_view name)
{
    auto promise = std::make_shared<std::promise<bool>>();
    futures[std::string(name)].emplace_back(promise->get_future());
    return [p = std::move(promise)]() { p->set_value(true); };
}

bool DbusEnvironment::waitForFuture(std::string_view name, Milliseconds timeout)
{
    return waitForFuture(getFuture(name), timeout);
}

bool DbusEnvironment::waitForFutures(std::string_view name,
                                     Milliseconds timeout)
{
    auto& data = futures[std::string(name)];
    auto ret = waitForFutures(
        std::move(data), true, [](auto sum, auto val) { return sum && val; },
        timeout);
    data = std::vector<std::future<bool>>{};
    return ret;
}

std::future<bool> DbusEnvironment::getFuture(std::string_view name)
{
    auto& data = futures[std::string(name)];
    auto it = data.begin();

    if (it != data.end())
    {
        auto result = std::move(*it);
        data.erase(it);
        return result;
    }

    return {};
}

void DbusEnvironment::sleepFor(Milliseconds timeout)
{
    auto end = std::chrono::high_resolution_clock::now() + timeout;

    while (std::chrono::high_resolution_clock::now() < end)
    {
        synchronizeIoc();
        std::this_thread::yield();
    }

    synchronizeIoc();
}

Milliseconds DbusEnvironment::measureTime(std::function<void()> fun)
{
    auto begin = std::chrono::high_resolution_clock::now();
    fun();
    auto end = std::chrono::high_resolution_clock::now();

    return std::chrono::duration_cast<Milliseconds>(end - begin);
}

boost::asio::io_context DbusEnvironment::ioc;
std::shared_ptr<sdbusplus::asio::connection> DbusEnvironment::bus;
std::shared_ptr<sdbusplus::asio::object_server> DbusEnvironment::objServer;
std::map<std::string, std::vector<std::future<bool>>> DbusEnvironment::futures;
bool DbusEnvironment::setUp = false;

std::thread DbusEnvironment::loggingThread;
std::atomic<bool> DbusEnvironment::loggingStop{false};

#include "trigger_actions.hpp"

#include "messages/update_report_ind.hpp"
#include "types/trigger_types.hpp"
#include "utils/clock.hpp"
#include "utils/messanger.hpp"
#include "utils/to_short_enum.hpp"

#include <phosphor-logging/log.hpp>

#include <ctime>
#include <iomanip>
#include <sstream>

namespace action
{

namespace
{
std::string timestampToString(Milliseconds timestamp)
{
    std::time_t t = static_cast<time_t>(
        std::chrono::duration_cast<std::chrono::seconds>(timestamp).count());
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&t), "%FT%T.") << std::setw(3)
       << std::setfill('0') << timestamp.count() % 1000 << 'Z';
    return ss.str();
}
} // namespace

namespace numeric
{

static const char* getDirection(double value, double threshold)
{
    if (value < threshold)
    {
        return "decreasing";
    }
    if (value > threshold)
    {
        return "increasing";
    }
    throw std::runtime_error("Invalid value");
}

static const char* getDbusSeverity(::numeric::Type type)
{
    switch (type)
    {
        case ::numeric::Type::upperCritical:
        case ::numeric::Type::lowerCritical:
            return redfish_message_ids::TriggerNumericCritical;
        case ::numeric::Type::upperWarning:
        case ::numeric::Type::lowerWarning:
            return redfish_message_ids::TriggerNumericWarning;
        default:
            return "xyz.openbmc_project.Logging.Entry.Level.Informational";
    }
}

const char* LogToRedfishEventLog::getRedfishMessageId(const double value) const
{
    std::string direction(getDirection(value, threshold));

    if (direction == "decreasing")
    {
        switch (type)
        {
            case ::numeric::Type::upperCritical:
                return redfish_message_ids::TriggerNumericBelowUpperCritical;
            case ::numeric::Type::lowerCritical:
                return redfish_message_ids::TriggerNumericBelowLowerCritical;
            case ::numeric::Type::upperWarning:
                return redfish_message_ids::TriggerNumericReadingNormal;
            case ::numeric::Type::lowerWarning:
                return redfish_message_ids::TriggerNumericBelowLowerWarning;
        }
    }

    if (direction == "increasing")
    {
        switch (type)
        {
            case ::numeric::Type::upperCritical:
                return redfish_message_ids::TriggerNumericAboveUpperCritical;
            case ::numeric::Type::lowerCritical:
                return redfish_message_ids::TriggerNumericAboveLowerCritical;
            case ::numeric::Type::upperWarning:
                return redfish_message_ids::TriggerNumericAboveUpperWarning;
            case ::numeric::Type::lowerWarning:
                return redfish_message_ids::TriggerNumericReadingNormal;
        }
    }

    throw std::runtime_error("Invalid type");
}

void LogToRedfishEventLog::commit(
    const std::string& triggerId, const ThresholdName thresholdNameInIn,
    const std::string& sensorName, const Milliseconds timestamp,
    const TriggerValue triggerValue)
{
    double value = std::get<double>(triggerValue);
    std::string thresholdName = ::numeric::typeToString(type);
    auto direction = getDirection(value, threshold);
    auto severity = getDbusSeverity(type);

    auto connection = sdbusplus::bus::new_default_system();
    sdbusplus::message_t AddToLog = connection.new_method_call(
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create");

    std::string journalMsg(
        "Numeric threshold '" + thresholdName + "' of trigger '" + triggerId +
        "' is crossed on sensor " + sensorName +
        ", recorded value: " + std::to_string(value) +
        ", crossing direction: " + std::string(utils::toShortEnum(direction)) +
        ", timestamp: " + timestampToString(timestamp));

    AddToLog.append(journalMsg, severity, std::map<std::string, std::string>());
    connection.call(AddToLog);
}

void fillActions(
    std::vector<std::unique_ptr<interfaces::TriggerAction>>& actionsIf,
    const std::vector<TriggerAction>& ActionsEnum, ::numeric::Type type,
    double thresholdValue, boost::asio::io_context& ioc,
    const std::shared_ptr<std::vector<std::string>>& reportIds)
{
    actionsIf.reserve(ActionsEnum.size());
    for (auto actionType : ActionsEnum)
    {
        switch (actionType)
        {
            case TriggerAction::LogToRedfishEventLog:
            {
                actionsIf.emplace_back(std::make_unique<LogToRedfishEventLog>(
                    type, thresholdValue));
                break;
            }
            case TriggerAction::UpdateReport:
            {
                actionsIf.emplace_back(
                    std::make_unique<UpdateReport>(ioc, reportIds));
                break;
            }
        }
    }
}

} // namespace numeric

namespace discrete
{

const char* LogToRedfishEventLog::getRedfishMessageId() const
{
    switch (severity)
    {
        case ::discrete::Severity::ok:
            return redfish_message_ids::TriggerDiscreteOK;
        case ::discrete::Severity::warning:
            return redfish_message_ids::TriggerDiscreteWarning;
        case ::discrete::Severity::critical:
            return redfish_message_ids::TriggerDiscreteCritical;
    }
    throw std::runtime_error("Invalid severity");
}

void LogToRedfishEventLog::commit(
    const std::string& triggerId, const ThresholdName thresholdNameIn,
    const std::string& sensorName, const Milliseconds timestamp,
    const TriggerValue triggerValue)
{
    auto value = std::get<std::string>(triggerValue);
    auto severity = getRedfishMessageId();

    auto connection = sdbusplus::bus::new_default_system();
    sdbusplus::message_t AddToLog = connection.new_method_call(
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create");

    std::string journalMsg(
        "Discrete condition '" + thresholdNameIn->get() + "' of trigger '" +
        triggerId + "' is crossed on sensor " + sensorName +
        ", recorded value: " + value +
        ", timestamp: " + timestampToString(timestamp));

    AddToLog.append(journalMsg, severity, std::map<std::string, std::string>());
    connection.call(AddToLog);
}

void fillActions(
    std::vector<std::unique_ptr<interfaces::TriggerAction>>& actionsIf,
    const std::vector<TriggerAction>& ActionsEnum,
    ::discrete::Severity severity, boost::asio::io_context& ioc,
    const std::shared_ptr<std::vector<std::string>>& reportIds)
{
    actionsIf.reserve(ActionsEnum.size());
    for (auto actionType : ActionsEnum)
    {
        switch (actionType)
        {
            case TriggerAction::LogToRedfishEventLog:
            {
                actionsIf.emplace_back(
                    std::make_unique<LogToRedfishEventLog>(severity));
                break;
            }
            case TriggerAction::UpdateReport:
            {
                actionsIf.emplace_back(
                    std::make_unique<UpdateReport>(ioc, reportIds));
                break;
            }
        }
    }
}

namespace onChange
{

void LogToRedfishEventLog::commit(
    const std::string& triggerId, const ThresholdName thresholdNameIn,
    const std::string& sensorName, const Milliseconds timestamp,
    const TriggerValue triggerValue)
{
    auto value = triggerValueToString(triggerValue);
    auto severity = redfish_message_ids::TriggerDiscreteOK;

    auto connection = sdbusplus::bus::new_default_system();
    sdbusplus::message_t AddToLog = connection.new_method_call(
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create");

    std::string journalMsg(
        "Discrete condition OnChange of trigger '" + triggerId +
        "' is crossed on sensor " + sensorName + ", recorded value: " + value +
        ", timestamp: " + timestampToString(timestamp));

    AddToLog.append(journalMsg, severity, std::map<std::string, std::string>());
    connection.call(AddToLog);
}

void fillActions(
    std::vector<std::unique_ptr<interfaces::TriggerAction>>& actionsIf,
    const std::vector<TriggerAction>& ActionsEnum, boost::asio::io_context& ioc,
    const std::shared_ptr<std::vector<std::string>>& reportIds)
{
    actionsIf.reserve(ActionsEnum.size());
    for (auto actionType : ActionsEnum)
    {
        switch (actionType)
        {
            case TriggerAction::LogToRedfishEventLog:
            {
                actionsIf.emplace_back(
                    std::make_unique<LogToRedfishEventLog>());
                break;
            }
            case TriggerAction::UpdateReport:
            {
                actionsIf.emplace_back(
                    std::make_unique<UpdateReport>(ioc, reportIds));
                break;
            }
        }
    }
}
} // namespace onChange
} // namespace discrete

void UpdateReport::commit(
    const std::string& triggerId, const ThresholdName thresholdNameIn,
    const std::string& sensorName, const Milliseconds timestamp,
    const TriggerValue triggerValue)
{
    if (reportIds->empty())
    {
        return;
    }

    utils::Messanger messanger(ioc);
    messanger.send(messages::UpdateReportInd{*reportIds});
}
} // namespace action

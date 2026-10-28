#include "operation_registry_devices.hpp"
#include "axklib/application/device_xml.hpp"
#include "device_format.hpp"
#include "device_wire.hpp"

#include <array>
#include <utility>

namespace axk::app {
const std::array<OperationDescriptor, 12U> &device_descriptors() {
    static const std::array descriptors{OperationDescriptor{"devices.inspect",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-inspections",
                                                            ExecutionMode::job,
                                                            {},
                                                            "DeviceInspectionRequest",
                                                            "DeviceInspection",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.sfs.plan",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-sfs-format-plans",
                                                            ExecutionMode::job,
                                                            {},
                                                            "DeviceSfsFormatPlanRequest",
                                                            "DeviceSfsFormatPlan",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.sfs.format",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-sfs-formats",
                                                            ExecutionMode::job,
                                                            {},
                                                            "DeviceSfsFormatRequest",
                                                            "DeviceSfsFormatResult",
                                                            OperationClass::write,
                                                            true,
                                                            false},
                                        OperationDescriptor{"devices.xml.inspect",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-xml-inspections",
                                                            ExecutionMode::request,
                                                            {},
                                                            "DeviceXmlInspectionRequest",
                                                            "DeviceXmlInspection",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.list",
                                                            {},
                                                            HttpMethod::get,
                                                            "/api/v1/devices",
                                                            ExecutionMode::request,
                                                            {},
                                                            "EmptyRequest",
                                                            "DeviceCandidateList",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.connect",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-connections",
                                                            ExecutionMode::job,
                                                            {},
                                                            "DeviceConnectRequest",
                                                            "DeviceSession",
                                                            OperationClass::write,
                                                            true,
                                                            false},
                                        OperationDescriptor{"devices.status",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-status",
                                                            ExecutionMode::request,
                                                            {},
                                                            "DeviceSessionRequest",
                                                            "DeviceSession",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.close",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-close",
                                                            ExecutionMode::request,
                                                            {},
                                                            "DeviceSessionRequest",
                                                            "DeviceCloseResult",
                                                            OperationClass::write,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.authorize",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-write-authorizations",
                                                            ExecutionMode::job,
                                                            {},
                                                            "DeviceAuthorizationRequest",
                                                            "DeviceAuthorizationResult",
                                                            OperationClass::write,
                                                            true,
                                                            false},
                                        OperationDescriptor{"devices.operations",
                                                            {},
                                                            HttpMethod::get,
                                                            "/api/v1/device-operations",
                                                            ExecutionMode::request,
                                                            {},
                                                            "EmptyRequest",
                                                            "DeviceOperationList",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.resolve",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-operation-status",
                                                            ExecutionMode::request,
                                                            {},
                                                            "DeviceOperationRequest",
                                                            "DeviceOperation",
                                                            OperationClass::read,
                                                            false,
                                                            false},
                                        OperationDescriptor{"devices.recover",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/device-recoveries",
                                                            ExecutionMode::job,
                                                            {},
                                                            "DeviceRecoveryRequest",
                                                            "DeviceRecoveryResult",
                                                            OperationClass::write,
                                                            true,
                                                            false}};
    return descriptors;
}

namespace {
using Json = nlohmann::json;
using device_detail::candidate_json;
Json session_json(const DeviceSessionSummary &session, bool write_supported) {
    auto targets = Json::array();
    for (const auto &target : session.targets)
        targets.push_back(device_detail::target_json(target, session.candidate));
    return {{"sessionId", session.session_id},
            {"candidate", candidate_json(session.candidate)},
            {"targets", targets},
            {"writesSupported", write_supported},
            {"writesAuthorized", session.writes_authorized},
            {"invalidated", session.invalidated}};
}
} // namespace

Result<void> bind_device_operations(OperationRegistry &registry, DeviceSessionManager &devices) {
    if (!devices.enabled())
        return {};
    const auto bind = [&](std::string_view id, OperationRegistry::Handler handler) {
        return registry.bind(id, std::move(handler));
    };
    if (auto bound = bind("devices.xml.inspect",
                          [](const Json &input, const OperationContext &) -> Result<Json> {
                              auto inspected = inspect_scsi2sd_xml(input.at("xml").get<std::string>());
                              if (!inspected)
                                  return std::unexpected(inspected.error());
                              auto targets = Json::array();
                              for (const auto &target : *inspected)
                                  targets.push_back({{"id", target.id},
                                                     {"label", target.label},
                                                     {"sizeBytes", target.size_bytes},
                                                     {"sectorSize", target.sector_size},
                                                     {"enabled", target.enabled},
                                                     {"eligible", target.eligible},
                                                     {"readOnly", target.read_only},
                                                     {"notices", target.notices}});
                              return Json{{"targets", targets}};
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.list",
                          [&devices](const Json &, const OperationContext &context) -> Result<Json> {
                              auto listed = devices.list(context.owner_id);
                              if (!listed)
                                  return std::unexpected(listed.error());
                              auto values = Json::array();
                              for (const auto &candidate : *listed)
                                  values.push_back(candidate_json(candidate));
                              return Json{{"devices", values}, {"writesSupported", devices.writes_supported()}};
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.connect",
                          [&devices](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto connected = devices.connect(input.at("candidateId").get<std::string>(),
                                                               context.owner_id, input.at("layout").get<std::string>(),
                                                               input.value("xml", std::string{}));
                              if (!connected)
                                  return std::unexpected(connected.error());
                              return session_json(*connected, devices.writes_supported());
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.status",
                          [&devices](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto session = devices.status(input.at("sessionId").get<std::string>(), context.owner_id);
                              if (!session)
                                  return std::unexpected(session.error());
                              return session_json(*session, devices.writes_supported());
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.close",
                          [&devices](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto closed = devices.close(input.at("sessionId").get<std::string>(), context.owner_id);
                              if (!closed)
                                  return std::unexpected(closed.error());
                              return Json{{"closed", true}};
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.authorize",
                          [&devices](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto result = devices.authorize_writes(
                                  input.at("sessionId").get<std::string>(), input.at("targetId").get<std::string>(),
                                  context.owner_id, input.at("exclusiveUseAcknowledged").get<bool>(),
                                  context.cancellation, context.progress);
                              if (!result)
                                  return std::unexpected(result.error());
                              return Json{{"authorized", true}};
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.operations",
                          [&devices](const Json &, const OperationContext &context) -> Result<Json> {
                              auto operations = devices.operations(context.owner_id);
                              if (!operations)
                                  return std::unexpected(operations.error());
                              auto values = Json::array();
                              for (const auto &operation : *operations)
                                  values.push_back({{"operationId", operation.operation_id},
                                                    {"kind", operation.kind},
                                                    {"state", operation.state},
                                                    {"model", operation.model},
                                                    {"serial", operation.serial},
                                                    {"targetLabel", operation.target_label},
                                                    {"targetSize", operation.target_size}});
                              return Json{{"operations", values}};
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.resolve",
                          [&devices](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto operation =
                                  devices.resolve(input.at("operationId").get<std::string>(), context.owner_id);
                              if (!operation)
                                  return std::unexpected(operation.error());
                              return Json{{"operationId", operation->operation_id},
                                          {"kind", operation->kind},
                                          {"state", operation->state},
                                          {"model", operation->model},
                                          {"serial", operation->serial},
                                          {"targetLabel", operation->target_label},
                                          {"targetSize", operation->target_size}};
                          });
        !bound)
        return bound;
    if (auto bound = bind("devices.recover",
                          [&devices](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto result = devices.recover(
                                  input.at("operationId").get<std::string>(), input.at("sessionId").get<std::string>(),
                                  input.at("targetId").get<std::string>(), context.owner_id,
                                  input.at("exclusiveUseAcknowledged").get<bool>(),
                                  input.value("confirmationText", std::string{}),
                                  input.value("dataLossAcknowledged", false), context.cancellation);
                              if (!result)
                                  return std::unexpected(result.error());
                              return Json{{"recovered", true}};
                          });
        !bound)
        return bound;
    return bind_device_format_operations(registry, devices);
}
} // namespace axk::app

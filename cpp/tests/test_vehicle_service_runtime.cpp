#include "sdv/vehicle_gateway.hpp"
#include "sdv/vehicle_service_runtime.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

sdv::ServiceRequest snapshot_request(std::int64_t deadline = 2'000) {
    return sdv::ServiceRequest{"request-1",
                               "trace-service-1",
                               "diagnostic-ui",
                               "vehicle.state",
                               "GetSnapshot",
                               deadline,
                               {"vehicle.state.read"},
                               sdv::GetVehicleSnapshotRequest{1'050, 100}};
}

void test_registration_discovery_and_typed_call() {
    std::int64_t now = 1'000;
    sdv::VehicleGateway gateway;
    const sdv::CanFrame status{sdv::VehicleGateway::kVehicleStatusId,
                               8U,
                               std::array<std::uint8_t, 8>{0x39, 0x30, 75, 0, 0, 0, 0, 0},
                               1'000,
                               "gateway-trace"};
    check(gateway.ingest(status).accepted, "gateway state prepared");

    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    check(sdv::register_vehicle_state_service(runtime, gateway), "vehicle service registered");
    check(!sdv::register_vehicle_state_service(runtime, gateway),
          "duplicate service registration rejected");
    const auto services = runtime.discover();
    check(services.size() == 1U && services.front().name == "vehicle.state",
          "registered service discovered");
    check(services.front().methods.front().required_permission == "vehicle.state.read",
          "method permission exposed in descriptor");

    const auto response = runtime.invoke(snapshot_request());
    check(response.accepted && response.code == "OK", "typed request accepted");
    const auto* snapshot = std::get_if<sdv::VehicleSnapshot>(&response.payload);
    check(snapshot != nullptr && snapshot->speed_kph == 123.45,
          "typed vehicle snapshot returned");
    check(runtime.trace_snapshot().size() == 2U, "dispatch and completion traced");
}

void test_admission_guards() {
    std::int64_t now = 1'000;
    sdv::VehicleGateway gateway;
    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    check(sdv::register_vehicle_state_service(runtime, gateway), "guard service registered");

    auto denied = snapshot_request();
    denied.permissions.clear();
    check(runtime.invoke(denied).code == "PERMISSION_DENIED", "permission enforced");

    auto missing_service = snapshot_request();
    missing_service.service = "vehicle.missing";
    check(runtime.invoke(missing_service).code == "SERVICE_NOT_FOUND", "unknown service rejected");

    auto missing_method = snapshot_request();
    missing_method.method = "Missing";
    check(runtime.invoke(missing_method).code == "METHOD_NOT_FOUND", "unknown method rejected");

    auto bad_payload = snapshot_request();
    bad_payload.request_id = "bad-payload";
    bad_payload.payload = sdv::SetDoorLocksRequest{0x0FU, true};
    check(runtime.invoke(bad_payload).code == "INVALID_PAYLOAD", "payload type enforced");

    auto expired = snapshot_request(999);
    expired.request_id = "expired-before-dispatch";
    check(runtime.invoke(expired).code == "DEADLINE_EXCEEDED", "pre-dispatch deadline enforced");

    auto cancelled = snapshot_request();
    cancelled.request_id = "cancel-before-dispatch";
    check(runtime.cancel(cancelled.request_id), "request cancellation recorded");
    check(runtime.invoke(cancelled).code == "CANCELLED", "pre-dispatch cancellation enforced");
}

void test_post_dispatch_result_uncertainty() {
    std::int64_t now = 1'000;
    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    const sdv::ServiceDescriptor slow{"vehicle.body", "1.0.0",
                                      {{"SetDoorLocks", "body.write", true}}};
    check(runtime.register_service(
              slow, [&now](const sdv::ServiceRequest& request) {
                  now = 1'101;
                  const auto* command = std::get_if<sdv::SetDoorLocksRequest>(&request.payload);
                  return sdv::ServiceResponse{
                      command != nullptr, command != nullptr ? "OK" : "INVALID_PAYLOAD",
                      "body command processed",
                      command != nullptr
                          ? sdv::ServiceResponsePayload{
                                sdv::DoorLockResult{command->door_mask, command->lock}}
                          : sdv::ServiceResponsePayload{std::monostate{}}};
              }),
          "slow body service registered");
    const sdv::ServiceRequest request{"slow-request",
                                      "slow-trace",
                                      "body-controller",
                                      "vehicle.body",
                                      "SetDoorLocks",
                                      1'100,
                                      {"body.write"},
                                      sdv::SetDoorLocksRequest{0x0FU, true}};
    const auto response = runtime.invoke(request);
    check(response.code == "COMPLETION_UNKNOWN" && !response.accepted,
          "post-dispatch timeout does not claim rollback");
    check(std::holds_alternative<std::monostate>(response.payload),
          "uncertain completion hides handler payload");
    now = 1'000;
    const auto replay = runtime.invoke(request);
    check(replay.code == "COMPLETION_UNKNOWN",
          "uncertain terminal result is replayed without re-dispatch");
}

void test_idempotent_replay_and_conflict() {
    std::int64_t now = 1'000;
    int executions = 0;
    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    const sdv::ServiceDescriptor body{"vehicle.body", "1.0.0",
                                      {{"SetDoorLocks", "body.write", true}}};
    check(runtime.register_service(
              body, [&executions](const sdv::ServiceRequest& request) {
                  ++executions;
                  const auto& command = std::get<sdv::SetDoorLocksRequest>(request.payload);
                  return sdv::ServiceResponse{
                      true, "OK", "door lock command applied",
                      sdv::DoorLockResult{command.door_mask, command.lock}};
              }),
          "idempotency body service registered");
    const sdv::ServiceRequest request{"body-command-1",
                                      "body-trace-1",
                                      "body-controller",
                                      "vehicle.body",
                                      "SetDoorLocks",
                                      2'000,
                                      {"body.write"},
                                      sdv::SetDoorLocksRequest{0x0FU, true}};
    const auto first = runtime.invoke(request);
    auto retry = request;
    retry.trace_id = "body-trace-retry";
    retry.deadline_ms = 3'000;
    const auto replay = runtime.invoke(retry);
    check(first.accepted && replay.accepted && executions == 1,
          "identical retry executes state-changing handler once");
    check(std::get<sdv::DoorLockResult>(replay.payload).requested_mask == 0x0FU,
          "identical retry returns original typed response");

    auto unauthorized_retry = retry;
    unauthorized_retry.permissions.clear();
    check(runtime.invoke(unauthorized_retry).code == "PERMISSION_DENIED",
          "cached response cannot bypass current permission check");
    check(executions == 1, "unauthorized retry does not reach handler");

    auto conflict = retry;
    conflict.payload = sdv::SetDoorLocksRequest{0x03U, false};
    check(runtime.invoke(conflict).code == "REQUEST_ID_CONFLICT",
          "same request id with different payload rejected");
    check(executions == 1, "conflicting retry does not reach handler");
}

sdv::ServiceRequest diagnostic_request(std::string id, std::string method,
                                       std::set<std::string> permissions,
                                       sdv::ServiceRequestPayload payload) {
    return sdv::ServiceRequest{std::move(id),
                               "diagnostic-trace",
                               "diagnostic-tool",
                               "vehicle.diagnostics",
                               std::move(method),
                               2'000,
                               std::move(permissions),
                               std::move(payload)};
}

void test_typed_diagnostic_service_flow() {
    std::int64_t now = 1'000;
    sdv::DiagnosticManager diagnostics;
    check(diagnostics.register_dtc(0xC10001U, "TPMS_LOW", 2U),
          "diagnostic service DTC registered");
    check(diagnostics.begin_operation_cycle(1U, 100, "cycle-1").accepted,
          "diagnostic service first cycle started");
    check(diagnostics.report_test_result(0xC10001U, true, sdv::VehicleSnapshot{}, 110,
                                         "failure-1")
              .accepted,
          "diagnostic service first failure reported");
    check(diagnostics.begin_operation_cycle(2U, 200, "cycle-2").accepted,
          "diagnostic service second cycle started");
    check(diagnostics.report_test_result(0xC10001U, true, sdv::VehicleSnapshot{}, 210,
                                         "failure-2")
              .accepted,
          "diagnostic service second failure reported");

    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    check(sdv::register_vehicle_diagnostic_service(runtime, diagnostics, [&now] { return now; }),
          "typed diagnostic service registered");
    const auto services = runtime.discover();
    check(services.size() == 1U && services.front().methods.size() == 5U,
          "five diagnostic methods discovered");

    const auto denied = runtime.invoke(diagnostic_request(
        "diag-read-denied", "ReadDTCInformation", {}, sdv::ReadDtcInformationRequest{true}));
    check(denied.code == "PERMISSION_DENIED", "diagnostic read permission enforced");

    const auto read = runtime.invoke(diagnostic_request(
        "diag-read", "ReadDTCInformation", {"diagnostic.read"},
        sdv::ReadDtcInformationRequest{true}));
    const auto* list = std::get_if<sdv::DiagnosticDtcListResult>(&read.payload);
    check(read.accepted && list != nullptr && list->dtcs.size() == 1U &&
              list->dtcs.front().confirmed,
          "typed diagnostic read returns confirmed DTC");

    const auto denied_clear = runtime.invoke(diagnostic_request(
        "diag-clear-denied", "ClearDiagnosticInformation", {"diagnostic.clear"},
        sdv::ClearDiagnosticInformationRequest{}));
    check(denied_clear.code == "SESSION_DENIED",
          "clear requires extended diagnostic session");

    const auto session = runtime.invoke(diagnostic_request(
        "diag-session", "StartSession", {"diagnostic.session"},
        sdv::StartDiagnosticSessionRequest{sdv::DiagnosticSession::Extended}));
    check(session.accepted, "extended session entered through runtime");
    const auto seed_response = runtime.invoke(diagnostic_request(
        "diag-seed", "RequestSecuritySeed", {"diagnostic.security"},
        sdv::RequestDiagnosticSeedRequest{}));
    const auto* seed = std::get_if<sdv::DiagnosticSeedResult>(&seed_response.payload);
    check(seed_response.accepted && seed != nullptr, "typed security seed returned");
    if (seed == nullptr) {
        return;
    }
    const auto key = runtime.invoke(diagnostic_request(
        "diag-key", "SubmitSecurityKey", {"diagnostic.security"},
        sdv::SubmitDiagnosticKeyRequest{seed->seed,
                                        sdv::DiagnosticManager::calculate_test_key(seed->seed)}));
    check(key.accepted, "security key accepted through runtime");

    auto clear_request = diagnostic_request(
        "diag-clear", "ClearDiagnosticInformation", {"diagnostic.clear"},
        sdv::ClearDiagnosticInformationRequest{});
    const auto cleared = runtime.invoke(clear_request);
    check(cleared.accepted && diagnostics.active_dtcs().empty(),
          "authorized clear reaches diagnostic manager");
    clear_request.trace_id = "diagnostic-clear-retry";
    check(runtime.invoke(clear_request).accepted,
          "identical clear retry replays terminal response");

    const auto empty = runtime.invoke(diagnostic_request(
        "diag-read-empty", "ReadDTCInformation", {"diagnostic.read"},
        sdv::ReadDtcInformationRequest{true}));
    const auto* empty_list = std::get_if<sdv::DiagnosticDtcListResult>(&empty.payload);
    check(empty_list != nullptr && empty_list->dtcs.empty(),
          "post-clear diagnostic read returns no active DTCs");

    const auto bad_payload = runtime.invoke(diagnostic_request(
        "diag-bad-payload", "ReadDTCInformation", {"diagnostic.read"},
        sdv::ClearDiagnosticInformationRequest{}));
    check(bad_payload.code == "INVALID_PAYLOAD", "diagnostic payload type enforced");
}

}  // namespace

int main() {
    test_registration_discovery_and_typed_call();
    test_admission_guards();
    test_post_dispatch_result_uncertainty();
    test_idempotent_replay_and_conflict();
    test_typed_diagnostic_service_flow();
    if (failures == 0) {
        std::cout << "All vehicle service runtime tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}

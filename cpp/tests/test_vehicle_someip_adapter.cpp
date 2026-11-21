#include "sdv/vehicle_gateway.hpp"
#include "sdv/vehicle_service_runtime.hpp"
#include "sdv/vehicle_someip_adapter.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

void test_runtime_adapter_round_trip() {
    std::int64_t clock_ms = 5'000;
    sdv::VehicleGateway gateway;
    const sdv::CanFrame status{sdv::VehicleGateway::kVehicleStatusId,
                               8U,
                               std::array<std::uint8_t, 8>{0x39, 0x30, 75, 1, 0, 0, 0, 0},
                               1'000,
                               "adapter-vcu"};
    check(gateway.ingest(status).accepted, "adapter gateway state seeded");
    sdv::VehicleServiceRuntime runtime([&clock_ms] { return clock_ms; });
    check(sdv::register_vehicle_state_service(runtime, gateway), "adapter service registered");

    const auto request = sdv::make_snapshot_someip_request(66U, 7U, 1'050, 100);
    const auto response = sdv::handle_vehicle_someip_request(request, runtime);
    check(response.client_id == 66U && response.session_id == 7U,
          "SOME/IP request identity preserved");
    sdv::VehicleSnapshotWireView snapshot;
    std::string error;
    check(sdv::decode_snapshot_someip_response(response, snapshot, error),
          "snapshot response decoded");
    check(snapshot.speed_kph == 123.45 && snapshot.battery_percent == 75,
          "snapshot payload values preserved");
    check(!snapshot.all_signal_groups_fresh,
          "missing signal groups represented as not all fresh");
    check(runtime.trace_snapshot().size() == 2U, "wire request traversed service runtime");
}

void test_adapter_errors() {
    std::int64_t clock_ms = 1'000;
    sdv::VehicleGateway gateway;
    sdv::VehicleServiceRuntime runtime([&clock_ms] { return clock_ms; });
    check(sdv::register_vehicle_state_service(runtime, gateway), "error service registered");
    auto request = sdv::make_snapshot_someip_request(1U, 2U, 10, 10);
    request.service_id = 0xFFFFU;
    check(sdv::handle_vehicle_someip_request(request, runtime).return_code ==
              sdv::SomeIpReturnCode::UnknownService,
          "unknown wire service rejected");
    request.service_id = sdv::kVehicleStateSomeIpService;
    request.method_id = 0x7777U;
    check(sdv::handle_vehicle_someip_request(request, runtime).return_code ==
              sdv::SomeIpReturnCode::UnknownMethod,
          "unknown wire method rejected");
    request.method_id = sdv::kGetSnapshotSomeIpMethod;
    request.payload.pop_back();
    check(sdv::handle_vehicle_someip_request(request, runtime).return_code ==
              sdv::SomeIpReturnCode::MalformedMessage,
          "invalid wire payload rejected");
}

void test_body_command_adapter() {
    std::int64_t clock_ms = 1'000;
    int executions = 0;
    sdv::VehicleServiceRuntime runtime([&clock_ms] { return clock_ms; });
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
          "body wire service registered");
    const auto request = sdv::make_door_lock_someip_request(82U, 9U, 0x0FU, true);
    const auto response = sdv::handle_vehicle_someip_request(request, runtime);
    sdv::DoorLockWireView result;
    std::string error;
    check(sdv::decode_door_lock_someip_response(response, result, error),
          "body command response decoded");
    check(result.requested_mask == 0x0FU && result.locked,
          "body command payload preserved");
    const auto replay = sdv::handle_vehicle_someip_request(request, runtime);
    check(sdv::decode_door_lock_someip_response(replay, result, error) && executions == 1,
          "wire retry is idempotent inside runtime");
}

void test_diagnostic_wire_adapter() {
    std::int64_t clock_ms = 1'000;
    sdv::DiagnosticManager diagnostics;
    check(diagnostics.register_dtc(0xC30001U, "WIRE_DTC", 3U),
          "wire diagnostic DTC registered");
    check(diagnostics.begin_operation_cycle(1U, 100, "wire-cycle-1").accepted,
          "wire diagnostic first cycle started");
    check(diagnostics.report_test_result(0xC30001U, true, sdv::VehicleSnapshot{}, 110,
                                         "wire-failure-1")
              .accepted,
          "wire diagnostic first failure reported");
    check(diagnostics.begin_operation_cycle(2U, 200, "wire-cycle-2").accepted,
          "wire diagnostic second cycle started");
    check(diagnostics.report_test_result(0xC30001U, true, sdv::VehicleSnapshot{}, 210,
                                         "wire-failure-2")
              .accepted,
          "wire diagnostic second failure reported");
    sdv::VehicleServiceRuntime runtime([&clock_ms] { return clock_ms; });
    check(sdv::register_vehicle_diagnostic_service(runtime, diagnostics,
                                                   [&clock_ms] { return clock_ms; }),
          "wire diagnostic service registered");

    sdv::DiagnosticWireResponse decoded;
    std::string error;
    auto response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_read_someip_request(0x61U, 1U, true), runtime);
    check(sdv::decode_diagnostic_someip_response(
              response, sdv::kReadDtcInformationSomeIpMethod, decoded, error) &&
              decoded.status == sdv::DiagnosticWireStatus::Ok && decoded.dtcs.size() == 1U &&
              decoded.dtcs.front().code == 0xC30001U && decoded.dtcs.front().confirmed &&
              decoded.dtcs.front().occurrence_count == 2U,
          "wire DTC record round trip preserves bounded fields");

    response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_clear_someip_request(0x61U, 2U), runtime);
    check(sdv::decode_diagnostic_someip_response(
              response, sdv::kClearDiagnosticInformationSomeIpMethod, decoded, error) &&
              decoded.status == sdv::DiagnosticWireStatus::SessionDenied,
          "wire clear exposes application session denial");

    response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_session_someip_request(
            0x61U, 3U, sdv::DiagnosticSession::Extended),
        runtime);
    check(sdv::decode_diagnostic_someip_response(
              response, sdv::kStartDiagnosticSessionSomeIpMethod, decoded, error) &&
              decoded.status == sdv::DiagnosticWireStatus::Ok,
          "wire extended session accepted");
    response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_seed_someip_request(0x61U, 4U), runtime);
    check(sdv::decode_diagnostic_someip_response(
              response, sdv::kRequestDiagnosticSeedSomeIpMethod, decoded, error) &&
              decoded.status == sdv::DiagnosticWireStatus::Ok && decoded.seed != 0U,
          "wire security seed returned");
    const auto seed = decoded.seed;
    response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_key_someip_request(
            0x61U, 5U, seed, sdv::DiagnosticManager::calculate_test_key(seed)),
        runtime);
    check(sdv::decode_diagnostic_someip_response(
              response, sdv::kSubmitDiagnosticKeySomeIpMethod, decoded, error) &&
              decoded.status == sdv::DiagnosticWireStatus::Ok,
          "wire security key accepted");
    response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_clear_someip_request(0x61U, 6U), runtime);
    check(sdv::decode_diagnostic_someip_response(
              response, sdv::kClearDiagnosticInformationSomeIpMethod, decoded, error) &&
              decoded.status == sdv::DiagnosticWireStatus::Ok &&
              diagnostics.active_dtcs().empty(),
          "wire authorized clear reaches diagnostic manager");

    auto malformed = sdv::make_diagnostic_key_someip_request(0x61U, 7U, 1U, 2U);
    malformed.payload.pop_back();
    check(sdv::handle_vehicle_someip_request(malformed, runtime).return_code ==
              sdv::SomeIpReturnCode::MalformedMessage,
          "wire malformed diagnostic key rejected before runtime");
}

}  // namespace

int main() {
    test_runtime_adapter_round_trip();
    test_adapter_errors();
    test_body_command_adapter();
    test_diagnostic_wire_adapter();
    if (failures == 0) {
        std::cout << "All vehicle SOME/IP adapter tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}

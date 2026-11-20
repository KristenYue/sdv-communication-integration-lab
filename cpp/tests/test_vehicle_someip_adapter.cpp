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

}  // namespace

int main() {
    test_runtime_adapter_round_trip();
    test_adapter_errors();
    if (failures == 0) {
        std::cout << "All vehicle SOME/IP adapter tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}

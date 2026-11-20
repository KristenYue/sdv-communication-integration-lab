#include "sdv/vehicle_gateway.hpp"
#include "sdv/vehicle_service_runtime.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

int main() {
    sdv::VehicleGateway gateway;
    const sdv::CanFrame status{sdv::VehicleGateway::kVehicleStatusId,
                               8U,
                               std::array<std::uint8_t, 8>{0x10, 0x27, 80, 0, 0, 0, 0, 0},
                               1'000,
                               "runtime-demo-frame"};
    if (!gateway.ingest(status).accepted) {
        return EXIT_FAILURE;
    }

    auto clock = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    sdv::VehicleServiceRuntime runtime(clock);
    if (!sdv::register_vehicle_state_service(runtime, gateway)) {
        return EXIT_FAILURE;
    }
    const auto deadline = clock() + 1'000;
    const sdv::ServiceRequest request{"demo-request",
                                      "demo-service-trace",
                                      "diagnostic-ui",
                                      "vehicle.state",
                                      "GetSnapshot",
                                      deadline,
                                      {"vehicle.state.read"},
                                      sdv::GetVehicleSnapshotRequest{1'050, 100}};
    const auto response = runtime.invoke(request);
    const auto* snapshot = std::get_if<sdv::VehicleSnapshot>(&response.payload);
    if (!response.accepted || snapshot == nullptr) {
        return EXIT_FAILURE;
    }
    std::cout << "service=vehicle.state/GetSnapshot\n"
              << "code=" << response.code << '\n'
              << "speed_kph=" << snapshot->speed_kph << '\n'
              << "runtime_trace_events=" << runtime.trace_snapshot().size() << '\n';
    return EXIT_SUCCESS;
}

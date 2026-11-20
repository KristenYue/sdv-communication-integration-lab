#include "sdv/fault_injector.hpp"
#include "sdv/vehicle_gateway.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

sdv::CanFrame status_frame(std::int64_t time_ms, std::string trace_id) {
    return sdv::CanFrame{sdv::VehicleGateway::kVehicleStatusId,
                         8U,
                         std::array<std::uint8_t, 8>{0x10, 0x27, 80, 0, 0, 0, 0, 0},
                         time_ms,
                         std::move(trace_id)};
}

}  // namespace

int main() {
    sdv::VehicleGateway gateway;
    const auto first = gateway.ingest(status_frame(1'000, "demo-vehicle-status"));
    const auto duplicate = gateway.ingest(status_frame(1'000, "demo-duplicate"));
    const auto snapshot = gateway.snapshot(1'050, 100);

    std::cout << "first=" << first.code << '\n'
              << "duplicate=" << duplicate.code << '\n'
              << "speed_kph=" << snapshot.speed_kph << '\n'
              << "battery_percent=" << snapshot.battery_percent << '\n'
              << "status_stale=" << (snapshot.vehicle_status.stale ? "true" : "false") << '\n'
              << "trace_events=" << gateway.trace().size() << '\n';
    return first.accepted && !duplicate.accepted ? EXIT_SUCCESS : EXIT_FAILURE;
}


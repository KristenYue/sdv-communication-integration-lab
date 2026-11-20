#include "sdv/fault_injector.hpp"
#include "sdv/vehicle_gateway.hpp"

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

sdv::CanFrame status(std::int64_t timestamp, std::string trace = "test-status") {
    return sdv::CanFrame{0x100,
                         8U,
                         std::array<std::uint8_t, 8>{0x39, 0x30, 75, 1, 0, 0, 0, 0},
                         timestamp,
                         std::move(trace)};
}

sdv::CanFrame tires(std::int64_t timestamp, std::string trace = "test-tires") {
    return sdv::CanFrame{0x101,
                         8U,
                         std::array<std::uint8_t, 8>{230, 0, 231, 0, 229, 0, 232, 0},
                         timestamp,
                         std::move(trace)};
}

void test_decode_and_freshness() {
    sdv::VehicleGateway gateway;
    check(gateway.ingest(status(1'000)).accepted, "vehicle status accepted");
    check(gateway.ingest(tires(1'005)).accepted, "tire pressure accepted");
    const auto fresh = gateway.snapshot(1'050, 100);
    check(fresh.speed_kph == 123.45, "speed scaling decoded");
    check(fresh.battery_percent == 75, "battery decoded");
    check(fresh.door_open, "door decoded");
    check(fresh.tire_kpa.at(2) == 229, "tire pressure decoded");
    check(!fresh.vehicle_status.stale && !fresh.tire_pressure.stale,
          "recent signal groups are fresh");
    const auto aged = gateway.snapshot(2'000, 100);
    check(aged.vehicle_status.stale && aged.tire_pressure.stale,
          "old signal groups become stale independently");
}

void test_temporal_and_contract_guards() {
    sdv::VehicleGateway gateway;
    check(gateway.ingest(status(100)).accepted, "initial frame accepted");
    check(gateway.ingest(status(100, "duplicate")).code == "DUPLICATE_FRAME",
          "duplicate rejected");
    auto conflict = status(100, "conflict");
    conflict.data.at(2) = 74;
    check(gateway.ingest(conflict).code == "TIMESTAMP_CONFLICT",
          "same timestamp payload conflict rejected");
    check(gateway.ingest(status(99, "stale")).code == "STALE_FRAME",
          "late frame cannot overwrite current state");
    auto bad_dlc = tires(101, "bad-dlc");
    bad_dlc.dlc = 7;
    check(gateway.ingest(bad_dlc).code == "INVALID_DLC", "wrong DLC rejected");
    auto unknown = tires(101, "unknown-id");
    unknown.id = 0x777;
    check(gateway.ingest(unknown).code == "UNKNOWN_CAN_ID", "unknown identifier rejected");
    check(gateway.trace().size() == 6U, "accepted and rejected paths are traced");
}

void test_signal_boundaries() {
    sdv::VehicleGateway gateway;
    auto battery = status(1);
    battery.data.at(2) = 101;
    check(gateway.ingest(battery).code == "SIGNAL_OUT_OF_RANGE",
          "battery range enforced");
    auto pressure = tires(2);
    pressure.data.at(0) = 0xE9;
    pressure.data.at(1) = 0x03;
    check(gateway.ingest(pressure).code == "SIGNAL_OUT_OF_RANGE",
          "tire pressure project boundary enforced");
}

void test_deterministic_faults() {
    const auto frame = status(1'000);
    sdv::DeterministicFaultInjector drop({sdv::FaultKind::Drop, 2, 0});
    check(drop.apply(frame, 1).size() == 1U, "non-target frame passes");
    check(drop.apply(frame, 2).empty(), "target frame drops");
    sdv::DeterministicFaultInjector duplicate({sdv::FaultKind::Duplicate, 1, 0});
    check(duplicate.apply(frame, 1).size() == 2U, "duplicate emits two frames");
    sdv::DeterministicFaultInjector corrupt({sdv::FaultKind::CorruptDlc, 1, 0});
    check(corrupt.apply(frame, 1).front().dlc == 7U, "DLC corruption is deterministic");
    sdv::DeterministicFaultInjector stale({sdv::FaultKind::StaleTimestamp, 1, 50});
    check(stale.apply(frame, 1).front().bus_time_ms == 950,
          "timestamp mutation is deterministic");
}

}  // namespace

int main() {
    test_decode_and_freshness();
    test_temporal_and_contract_guards();
    test_signal_boundaries();
    test_deterministic_faults();
    if (failures == 0) {
        std::cout << "All SDV core tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}


#include "sdv/gateway_diagnostic_bridge.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
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

sdv::CanFrame frame(std::uint32_t id, std::array<std::uint8_t, 8> data,
                    std::int64_t time_ms, std::string trace, std::uint8_t dlc = 8U) {
    return {id, dlc, data, time_ms, std::move(trace)};
}

void observe(sdv::VehicleGateway& gateway, sdv::GatewayDiagnosticBridge& bridge,
             const sdv::CanFrame& value) {
    check(bridge.observe_ingest(value, gateway.ingest(value)), "gateway result observed");
}

void healthy_frames(sdv::VehicleGateway& gateway, sdv::GatewayDiagnosticBridge& bridge,
                    std::int64_t base) {
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kVehicleStatusId,
                  {0x10, 0x27, 80, 0, 0, 0, 0, 0}, base, "healthy-vcu"));
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kTirePressureId,
                  {230, 0, 231, 0, 229, 0, 232, 0}, base + 1, "healthy-tpms"));
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kBodyStatusId,
                  {0, 15, 1, 0, 0, 0, 0, 0}, base + 2, "healthy-bcm"));
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kTboxHealthId,
                  {1, 88, 0, 0, 0, 0, 0, 0}, base + 3, "healthy-tbox"));
}

void inject_contract_faults(sdv::VehicleGateway& gateway,
                            sdv::GatewayDiagnosticBridge& bridge,
                            std::int64_t base) {
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kBodyStatusId, {}, base + 10, "fault-dlc", 7U));
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kVehicleStatusId,
                  {0x0C, 0x62, 80, 0, 0, 0, 0, 0}, base + 11, "fault-range"));
    observe(gateway, bridge,
            frame(sdv::VehicleGateway::kTirePressureId,
                  {230, 0, 231, 0, 229, 0, 232, 0}, base, "fault-stale"));
    observe(gateway, bridge, frame(0x777U, {}, base + 12, "fault-unknown"));
}

void test_gateway_fault_lifecycle() {
    sdv::VehicleGateway gateway;
    sdv::DiagnosticManager diagnostics;
    sdv::GatewayDiagnosticBridge bridge(gateway, diagnostics);
    check(bridge.initialize(), "bridge definitions initialized");
    check(bridge.initialize(), "bridge initialization is idempotent");

    for (std::uint64_t cycle = 1U; cycle <= 2U; ++cycle) {
        const auto base = static_cast<std::int64_t>(cycle * 1'000U);
        check(bridge.begin_cycle(cycle, base, "bridge-cycle").accepted,
              "fault cycle started");
        healthy_frames(gateway, bridge, base);
        inject_contract_faults(gateway, bridge, base);
        const auto result = bridge.close_cycle(base + 500, 100, "bridge-close");
        check(result.accepted && result.monitors_reported == 8U &&
                  result.failed_monitors == 8U,
              "all contract and timeout monitors fail once per cycle");
    }
    const auto confirmed = diagnostics.active_dtcs();
    check(confirmed.size() == 8U, "eight gateway DTCs active");
    for (const auto& dtc : confirmed) {
        check(dtc.confirmed && dtc.occurrence_count == 2U && dtc.freeze_frame.valid,
              "gateway DTC confirmed with bounded occurrence and freeze frame");
    }

    for (std::uint64_t cycle = 3U; cycle <= 5U; ++cycle) {
        const auto base = static_cast<std::int64_t>(cycle * 1'000U);
        check(bridge.begin_cycle(cycle, base, "recovery-cycle").accepted,
              "recovery cycle started");
        healthy_frames(gateway, bridge, base);
        const auto result = bridge.close_cycle(base + 10, 100, "recovery-close");
        check(result.accepted && result.failed_monitors == 0U,
              "healthy cycle reports every monitor passing");
    }
    check(diagnostics.active_dtcs().empty(), "three healthy cycles age gateway DTCs");
}

}  // namespace

int main() {
    test_gateway_fault_lifecycle();
    if (failures == 0) {
        std::cout << "All gateway diagnostic bridge tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}

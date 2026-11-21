#include "sdv/gateway_diagnostic_bridge.hpp"

#include <array>
#include <utility>

namespace sdv {
namespace {

struct MonitorDefinition {
    std::uint32_t code;
    const char* name;
    std::uint8_t severity;
};

constexpr std::array<MonitorDefinition, GatewayDiagnosticBridge::kMonitorCount> kMonitors{{
    {GatewayDiagnosticBridge::kInvalidDlcDtc, "CAN_INVALID_DLC", 2U},
    {GatewayDiagnosticBridge::kSignalRangeDtc, "CAN_SIGNAL_OUT_OF_RANGE", 2U},
    {GatewayDiagnosticBridge::kFrameSequenceDtc, "CAN_FRAME_SEQUENCE_ERROR", 2U},
    {GatewayDiagnosticBridge::kUnknownCanIdDtc, "CAN_UNKNOWN_IDENTIFIER", 1U},
    {GatewayDiagnosticBridge::kVehicleStatusTimeoutDtc, "VCU_STATUS_TIMEOUT", 3U},
    {GatewayDiagnosticBridge::kTirePressureTimeoutDtc, "TPMS_STATUS_TIMEOUT", 3U},
    {GatewayDiagnosticBridge::kBodyStatusTimeoutDtc, "BCM_STATUS_TIMEOUT", 3U},
    {GatewayDiagnosticBridge::kTboxHealthTimeoutDtc, "TBOX_STATUS_TIMEOUT", 3U},
}};

}  // namespace

GatewayDiagnosticBridge::GatewayDiagnosticBridge(VehicleGateway& gateway,
                                                 DiagnosticManager& diagnostics)
    : gateway_(gateway), diagnostics_(diagnostics) {}

bool GatewayDiagnosticBridge::initialize() {
    if (initialized_) {
        return true;
    }
    for (const auto& monitor : kMonitors) {
        if (!diagnostics_.register_dtc(monitor.code, monitor.name, monitor.severity)) {
            return false;
        }
    }
    initialized_ = true;
    return true;
}

DiagnosticOutcome GatewayDiagnosticBridge::begin_cycle(std::uint64_t cycle_id,
                                                       std::int64_t now_ms,
                                                       std::string trace_id) {
    if (!initialized_ || cycle_active_) {
        return {false, "BRIDGE_STATE_ERROR",
                "bridge must be initialized and the previous cycle closed", 0U};
    }
    auto outcome = diagnostics_.begin_operation_cycle(cycle_id, now_ms, std::move(trace_id));
    if (outcome.accepted) {
        failed_.fill(false);
        fault_traces_.fill({});
        cycle_active_ = true;
    }
    return outcome;
}

bool GatewayDiagnosticBridge::observe_ingest(const CanFrame& frame,
                                             const IngestResult& result) {
    if (!cycle_active_) {
        return false;
    }
    std::size_t index = kMonitorCount;
    if (result.code == "INVALID_DLC") {
        index = 0U;
    } else if (result.code == "SIGNAL_OUT_OF_RANGE") {
        index = 1U;
    } else if (result.code == "STALE_FRAME" || result.code == "TIMESTAMP_CONFLICT") {
        index = 2U;
    } else if (result.code == "UNKNOWN_CAN_ID") {
        index = 3U;
    }
    if (index < kMonitorCount) {
        failed_.at(index) = true;
        if (fault_traces_.at(index).empty()) {
            fault_traces_.at(index) = frame.trace_id;
        }
    }
    return true;
}

GatewayDiagnosticCycleResult GatewayDiagnosticBridge::close_cycle(
    std::int64_t now_ms, std::int64_t freshness_limit_ms, std::string trace_id) {
    if (!initialized_ || !cycle_active_ || now_ms < 0 || trace_id.empty() ||
        trace_id.size() > 48U) {
        return {false, "BRIDGE_STATE_ERROR", 0U, 0U};
    }
    const auto snapshot = gateway_.snapshot(now_ms, freshness_limit_ms);
    failed_.at(4U) = snapshot.vehicle_status.stale;
    failed_.at(5U) = snapshot.tire_pressure.stale;
    failed_.at(6U) = snapshot.body_status.stale;
    failed_.at(7U) = snapshot.tbox_health.stale;

    GatewayDiagnosticCycleResult result{true, "OK", 0U, 0U};
    for (std::size_t index = 0; index < kMonitors.size(); ++index) {
        const auto failed = failed_.at(index);
        const auto monitor_trace = fault_traces_.at(index).empty()
                                       ? trace_id + "-monitor-" + std::to_string(index)
                                       : fault_traces_.at(index);
        const auto outcome = diagnostics_.report_test_result(
            kMonitors.at(index).code, failed, snapshot, now_ms, monitor_trace);
        if (!outcome.accepted) {
            result.accepted = false;
            result.code = outcome.code;
            break;
        }
        ++result.monitors_reported;
        result.failed_monitors += failed ? 1U : 0U;
    }
    cycle_active_ = false;
    return result;
}

}  // namespace sdv

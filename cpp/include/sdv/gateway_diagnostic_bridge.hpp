#pragma once

#include "sdv/diagnostic_manager.hpp"
#include "sdv/vehicle_gateway.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sdv {

struct GatewayDiagnosticCycleResult {
    bool accepted{false};
    std::string code;
    std::size_t monitors_reported{0};
    std::size_t failed_monitors{0};
};

class GatewayDiagnosticBridge {
public:
    static constexpr std::uint32_t kInvalidDlcDtc = 0xC51001U;
    static constexpr std::uint32_t kSignalRangeDtc = 0xC51002U;
    static constexpr std::uint32_t kFrameSequenceDtc = 0xC51003U;
    static constexpr std::uint32_t kUnknownCanIdDtc = 0xC51004U;
    static constexpr std::uint32_t kVehicleStatusTimeoutDtc = 0xC51101U;
    static constexpr std::uint32_t kTirePressureTimeoutDtc = 0xC51102U;
    static constexpr std::uint32_t kBodyStatusTimeoutDtc = 0xC51103U;
    static constexpr std::uint32_t kTboxHealthTimeoutDtc = 0xC51104U;
    static constexpr std::size_t kMonitorCount = 8U;

    GatewayDiagnosticBridge(VehicleGateway& gateway, DiagnosticManager& diagnostics);

    bool initialize();
    DiagnosticOutcome begin_cycle(std::uint64_t cycle_id, std::int64_t now_ms,
                                  std::string trace_id);
    bool observe_ingest(const CanFrame& frame, const IngestResult& result);
    GatewayDiagnosticCycleResult close_cycle(std::int64_t now_ms,
                                             std::int64_t freshness_limit_ms,
                                             std::string trace_id);

private:
    VehicleGateway& gateway_;
    DiagnosticManager& diagnostics_;
    bool initialized_{false};
    bool cycle_active_{false};
    std::array<bool, kMonitorCount> failed_{};
    std::array<std::string, kMonitorCount> fault_traces_{};
};

}  // namespace sdv

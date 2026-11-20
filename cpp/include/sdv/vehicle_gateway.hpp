#pragma once

#include "sdv/types.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace sdv {

class VehicleGateway {
public:
    static constexpr std::uint32_t kVehicleStatusId = 0x100;
    static constexpr std::uint32_t kTirePressureId = 0x101;
    static constexpr std::uint32_t kBodyStatusId = 0x102;
    static constexpr std::uint32_t kTboxHealthId = 0x103;

    IngestResult ingest(const CanFrame& frame);
    VehicleSnapshot snapshot(std::int64_t now_ms, std::int64_t max_age_ms) const;
    const std::vector<TraceEvent>& trace() const noexcept { return trace_; }

private:
    struct LastFrame {
        std::int64_t bus_time_ms{0};
        std::array<std::uint8_t, 8> data{};
    };

    VehicleSnapshot state_;
    std::map<std::uint32_t, LastFrame> last_frames_;
    std::vector<TraceEvent> trace_;
    std::uint64_t next_sequence_{1};

    IngestResult reject(const CanFrame& frame, std::string code, std::string message);
    void record(const CanFrame& frame, std::string event, std::string detail);
};

}  // namespace sdv


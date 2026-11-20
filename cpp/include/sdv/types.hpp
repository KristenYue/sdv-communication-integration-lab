#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace sdv {

struct CanFrame {
    std::uint32_t id{0};
    std::uint8_t dlc{0};
    std::array<std::uint8_t, 8> data{};
    std::int64_t bus_time_ms{0};
    std::string trace_id;
};

struct IngestResult {
    bool accepted{false};
    std::string code;
    std::string message;
};

struct SignalGroupState {
    bool available{false};
    bool stale{true};
    std::int64_t bus_time_ms{0};
};

struct VehicleSnapshot {
    SignalGroupState vehicle_status;
    SignalGroupState tire_pressure;
    SignalGroupState body_status;
    SignalGroupState tbox_health;
    double speed_kph{0.0};
    int battery_percent{0};
    bool door_open{false};
    std::array<int, 4> tire_kpa{};
    std::uint8_t door_open_mask{0};
    std::uint8_t door_lock_mask{0};
    std::uint8_t wiper_state{0};
    bool cloud_connected{false};
    int signal_quality_percent{0};
    int upload_backlog{0};
};

struct TraceEvent {
    std::uint64_t sequence{0};
    std::int64_t bus_time_ms{0};
    std::string trace_id;
    std::string event;
    std::string detail;
};

}  // namespace sdv


#include "sdv/vehicle_gateway.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace sdv {
namespace {

std::uint16_t u16_le(const std::array<std::uint8_t, 8>& data, std::size_t offset) {
    return static_cast<std::uint16_t>(data.at(offset)) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data.at(offset + 1)) << 8U);
}

bool same_payload(const std::array<std::uint8_t, 8>& left,
                  const std::array<std::uint8_t, 8>& right) {
    return std::equal(left.begin(), left.end(), right.begin());
}

}  // namespace

void VehicleGateway::record(const CanFrame& frame, std::string event, std::string detail) {
    trace_.push_back(TraceEvent{next_sequence_++, frame.bus_time_ms, frame.trace_id,
                                std::move(event), std::move(detail)});
}

IngestResult VehicleGateway::reject(const CanFrame& frame, std::string code,
                                    std::string message) {
    record(frame, code, message);
    return IngestResult{false, std::move(code), std::move(message)};
}

IngestResult VehicleGateway::ingest(const CanFrame& frame) {
    if (frame.trace_id.empty() || frame.trace_id.size() > 64U) {
        return reject(frame, "INVALID_TRACE_ID", "trace_id must contain 1..64 characters");
    }
    if (frame.id != kVehicleStatusId && frame.id != kTirePressureId &&
        frame.id != kBodyStatusId && frame.id != kTboxHealthId) {
        return reject(frame, "UNKNOWN_CAN_ID", "frame identifier is not registered");
    }
    if (frame.dlc != 8U) {
        return reject(frame, "INVALID_DLC", "registered frame requires DLC=8");
    }
    if (frame.bus_time_ms < 0) {
        return reject(frame, "INVALID_TIMESTAMP", "bus timestamp cannot be negative");
    }

    if (const auto previous = last_frames_.find(frame.id); previous != last_frames_.end()) {
        if (frame.bus_time_ms < previous->second.bus_time_ms) {
            return reject(frame, "STALE_FRAME", "older frame cannot overwrite current signal state");
        }
        if (frame.bus_time_ms == previous->second.bus_time_ms) {
            if (same_payload(frame.data, previous->second.data)) {
                return reject(frame, "DUPLICATE_FRAME", "identical frame was already applied");
            }
            return reject(frame, "TIMESTAMP_CONFLICT",
                          "same identifier and timestamp carry different payloads");
        }
    }

    if (frame.id == kVehicleStatusId) {
        const double speed = static_cast<double>(u16_le(frame.data, 0U)) * 0.01;
        const int battery = static_cast<int>(frame.data.at(2U));
        if (speed > 250.0) {
            return reject(frame, "SIGNAL_OUT_OF_RANGE", "speed exceeds project DBC range");
        }
        if (battery > 100) {
            return reject(frame, "SIGNAL_OUT_OF_RANGE", "battery exceeds 100 percent");
        }
        state_.speed_kph = speed;
        state_.battery_percent = battery;
        state_.door_open = (frame.data.at(3U) & 0x01U) != 0U;
        state_.vehicle_status = SignalGroupState{true, false, frame.bus_time_ms};
        record(frame, "VEHICLE_STATUS_APPLIED", "speed, battery and door signals updated");
    } else if (frame.id == kTirePressureId) {
        std::array<int, 4> pressures{};
        for (std::size_t index = 0; index < pressures.size(); ++index) {
            pressures.at(index) = static_cast<int>(u16_le(frame.data, index * 2U));
            if (pressures.at(index) < 0 || pressures.at(index) > 1000) {
                return reject(frame, "SIGNAL_OUT_OF_RANGE",
                              "tire pressure exceeds project safety boundary");
            }
        }
        state_.tire_kpa = pressures;
        state_.tire_pressure = SignalGroupState{true, false, frame.bus_time_ms};
        record(frame, "TIRE_PRESSURE_APPLIED", "four tire pressure signals updated");
    } else if (frame.id == kBodyStatusId) {
        const auto door_open_mask = static_cast<std::uint8_t>(frame.data.at(0U) & 0x0FU);
        const auto door_lock_mask = static_cast<std::uint8_t>(frame.data.at(1U) & 0x0FU);
        const auto wiper_state = frame.data.at(2U);
        if (wiper_state > 3U) {
            return reject(frame, "SIGNAL_OUT_OF_RANGE", "wiper state exceeds project DBC range");
        }
        state_.door_open_mask = door_open_mask;
        state_.door_lock_mask = door_lock_mask;
        state_.wiper_state = wiper_state;
        state_.body_status = SignalGroupState{true, false, frame.bus_time_ms};
        record(frame, "BODY_STATUS_APPLIED", "door and wiper signals updated");
    } else {
        const auto connected = frame.data.at(0U);
        const auto signal_quality = frame.data.at(1U);
        const auto backlog = static_cast<int>(u16_le(frame.data, 2U));
        if (connected > 1U) {
            return reject(frame, "SIGNAL_OUT_OF_RANGE", "cloud connectivity must be 0 or 1");
        }
        if (signal_quality > 100U) {
            return reject(frame, "SIGNAL_OUT_OF_RANGE", "signal quality exceeds 100 percent");
        }
        state_.cloud_connected = connected == 1U;
        state_.signal_quality_percent = static_cast<int>(signal_quality);
        state_.upload_backlog = backlog;
        state_.tbox_health = SignalGroupState{true, false, frame.bus_time_ms};
        record(frame, "TBOX_HEALTH_APPLIED", "connectivity and upload backlog updated");
    }

    last_frames_[frame.id] = LastFrame{frame.bus_time_ms, frame.data};
    return IngestResult{true, "APPLIED", "frame applied"};
}

VehicleSnapshot VehicleGateway::snapshot(std::int64_t now_ms, std::int64_t max_age_ms) const {
    VehicleSnapshot result = state_;
    const auto stale = [now_ms, max_age_ms](const SignalGroupState& group) {
        return !group.available || max_age_ms < 0 || now_ms < group.bus_time_ms ||
               now_ms - group.bus_time_ms > max_age_ms;
    };
    result.vehicle_status.stale = stale(result.vehicle_status);
    result.tire_pressure.stale = stale(result.tire_pressure);
    result.body_status.stale = stale(result.body_status);
    result.tbox_health.stale = stale(result.tbox_health);
    return result;
}

}  // namespace sdv


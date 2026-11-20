#pragma once

#include "sdv/someip.hpp"
#include "sdv/vehicle_service_runtime.hpp"

#include <cstdint>
#include <string>

namespace sdv {

constexpr std::uint16_t kVehicleStateSomeIpService = 0x1234U;
constexpr std::uint16_t kGetSnapshotSomeIpMethod = 0x0001U;
constexpr std::uint16_t kVehicleBodySomeIpService = 0x1235U;
constexpr std::uint16_t kSetDoorLocksSomeIpMethod = 0x0001U;

struct VehicleSnapshotWireView {
    double speed_kph{0.0};
    int battery_percent{0};
    bool door_open{false};
    bool all_signal_groups_fresh{false};
};

struct DoorLockWireView {
    std::uint8_t requested_mask{0};
    bool locked{false};
};

SomeIpMessage make_snapshot_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                           std::int64_t now_ms, std::int64_t max_age_ms);
SomeIpMessage make_door_lock_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                            std::uint8_t door_mask, bool lock);
SomeIpMessage handle_vehicle_someip_request(const SomeIpMessage& message,
                                            VehicleServiceRuntime& runtime);
bool decode_snapshot_someip_response(const SomeIpMessage& message, VehicleSnapshotWireView& output,
                                     std::string& error);
bool decode_door_lock_someip_response(const SomeIpMessage& message, DoorLockWireView& output,
                                      std::string& error);

}  // namespace sdv

#pragma once

#include "sdv/someip.hpp"
#include "sdv/vehicle_service_runtime.hpp"

#include <cstdint>
#include <string>

namespace sdv {

constexpr std::uint16_t kVehicleStateSomeIpService = 0x1234U;
constexpr std::uint16_t kGetSnapshotSomeIpMethod = 0x0001U;

struct VehicleSnapshotWireView {
    double speed_kph{0.0};
    int battery_percent{0};
    bool door_open{false};
    bool all_signal_groups_fresh{false};
};

SomeIpMessage make_snapshot_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                           std::int64_t now_ms, std::int64_t max_age_ms);
SomeIpMessage handle_vehicle_someip_request(const SomeIpMessage& message,
                                            VehicleServiceRuntime& runtime);
bool decode_snapshot_someip_response(const SomeIpMessage& message, VehicleSnapshotWireView& output,
                                     std::string& error);

}  // namespace sdv

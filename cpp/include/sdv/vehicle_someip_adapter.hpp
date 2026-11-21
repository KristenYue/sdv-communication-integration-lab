#pragma once

#include "sdv/someip.hpp"
#include "sdv/vehicle_service_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sdv {

constexpr std::uint16_t kVehicleStateSomeIpService = 0x1234U;
constexpr std::uint16_t kGetSnapshotSomeIpMethod = 0x0001U;
constexpr std::uint16_t kVehicleBodySomeIpService = 0x1235U;
constexpr std::uint16_t kSetDoorLocksSomeIpMethod = 0x0001U;
constexpr std::uint16_t kVehicleDiagnosticSomeIpService = 0x1236U;
constexpr std::uint16_t kStartDiagnosticSessionSomeIpMethod = 0x0001U;
constexpr std::uint16_t kRequestDiagnosticSeedSomeIpMethod = 0x0002U;
constexpr std::uint16_t kSubmitDiagnosticKeySomeIpMethod = 0x0003U;
constexpr std::uint16_t kReadDtcInformationSomeIpMethod = 0x0004U;
constexpr std::uint16_t kClearDiagnosticInformationSomeIpMethod = 0x0005U;

enum class DiagnosticWireStatus : std::uint8_t {
    Ok = 0U,
    SessionDenied = 1U,
    SecurityDenied = 2U,
    SecurityLocked = 3U,
    InvalidKey = 4U,
    ApplicationError = 5U,
};

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

struct DiagnosticDtcWireView {
    std::uint32_t code{0};
    std::uint8_t severity{0};
    bool test_failed{false};
    bool pending{false};
    bool confirmed{false};
    std::uint32_t occurrence_count{0};
};

struct DiagnosticWireResponse {
    DiagnosticWireStatus status{DiagnosticWireStatus::ApplicationError};
    std::uint32_t seed{0};
    std::vector<DiagnosticDtcWireView> dtcs;
};

SomeIpMessage make_snapshot_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                           std::int64_t now_ms, std::int64_t max_age_ms);
SomeIpMessage make_door_lock_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                            std::uint8_t door_mask, bool lock);
SomeIpMessage make_diagnostic_session_someip_request(std::uint16_t client_id,
                                                     std::uint16_t session_id,
                                                     DiagnosticSession session);
SomeIpMessage make_diagnostic_seed_someip_request(std::uint16_t client_id,
                                                  std::uint16_t session_id);
SomeIpMessage make_diagnostic_key_someip_request(std::uint16_t client_id,
                                                 std::uint16_t session_id,
                                                 std::uint32_t seed, std::uint32_t key);
SomeIpMessage make_diagnostic_read_someip_request(std::uint16_t client_id,
                                                  std::uint16_t session_id, bool active_only);
SomeIpMessage make_diagnostic_clear_someip_request(std::uint16_t client_id,
                                                   std::uint16_t session_id);
SomeIpMessage handle_vehicle_someip_request(const SomeIpMessage& message,
                                            VehicleServiceRuntime& runtime);
bool decode_snapshot_someip_response(const SomeIpMessage& message, VehicleSnapshotWireView& output,
                                     std::string& error);
bool decode_door_lock_someip_response(const SomeIpMessage& message, DoorLockWireView& output,
                                      std::string& error);
bool decode_diagnostic_someip_response(const SomeIpMessage& message,
                                       std::uint16_t expected_method,
                                       DiagnosticWireResponse& output, std::string& error);

}  // namespace sdv

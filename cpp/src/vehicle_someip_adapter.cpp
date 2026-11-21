#include "sdv/vehicle_someip_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sdv {
namespace {

void append_i64(std::vector<std::uint8_t>& bytes, std::int64_t value) {
    const auto raw = static_cast<std::uint64_t>(value);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((raw >> shift) & 0xFFU));
    }
}

std::int64_t read_i64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t raw = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        raw = (raw << 8U) | bytes.at(offset + index);
    }
    return static_cast<std::int64_t>(raw);
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) | bytes.at(offset + index);
    }
    return value;
}

void append_u24(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::uint32_t read_u24(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes.at(offset)) << 16U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U) |
           static_cast<std::uint32_t>(bytes.at(offset + 2U));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes.at(offset)) << 8U) |
           static_cast<std::uint16_t>(bytes.at(offset + 1U));
}

SomeIpMessage error_response(const SomeIpMessage& request, SomeIpReturnCode code) {
    return SomeIpMessage{request.service_id, request.method_id, request.client_id,
                         request.session_id, request.interface_version,
                         SomeIpMessageType::Error, code, {}};
}

DiagnosticWireStatus diagnostic_status(const ServiceResponse& response) {
    if (response.accepted) {
        return DiagnosticWireStatus::Ok;
    }
    if (response.code == "SESSION_DENIED") {
        return DiagnosticWireStatus::SessionDenied;
    }
    if (response.code == "SECURITY_DENIED") {
        return DiagnosticWireStatus::SecurityDenied;
    }
    if (response.code == "SECURITY_LOCKED") {
        return DiagnosticWireStatus::SecurityLocked;
    }
    if (response.code == "INVALID_KEY") {
        return DiagnosticWireStatus::InvalidKey;
    }
    return DiagnosticWireStatus::ApplicationError;
}

SomeIpMessage handle_diagnostic_request(const SomeIpMessage& message,
                                        VehicleServiceRuntime& runtime) {
    std::string method;
    std::string permission;
    ServiceRequestPayload request_payload;
    switch (message.method_id) {
        case kStartDiagnosticSessionSomeIpMethod:
            if (message.payload.size() != 1U || message.payload.front() > 1U) {
                return error_response(message, SomeIpReturnCode::MalformedMessage);
            }
            method = "StartSession";
            permission = "diagnostic.session";
            request_payload = StartDiagnosticSessionRequest{
                message.payload.front() == 1U ? DiagnosticSession::Extended
                                              : DiagnosticSession::Default};
            break;
        case kRequestDiagnosticSeedSomeIpMethod:
            if (!message.payload.empty()) {
                return error_response(message, SomeIpReturnCode::MalformedMessage);
            }
            method = "RequestSecuritySeed";
            permission = "diagnostic.security";
            request_payload = RequestDiagnosticSeedRequest{};
            break;
        case kSubmitDiagnosticKeySomeIpMethod:
            if (message.payload.size() != 8U) {
                return error_response(message, SomeIpReturnCode::MalformedMessage);
            }
            method = "SubmitSecurityKey";
            permission = "diagnostic.security";
            request_payload =
                SubmitDiagnosticKeyRequest{read_u32(message.payload, 0U),
                                           read_u32(message.payload, 4U)};
            break;
        case kReadDtcInformationSomeIpMethod:
            if (message.payload.size() != 1U || message.payload.front() > 1U) {
                return error_response(message, SomeIpReturnCode::MalformedMessage);
            }
            method = "ReadDTCInformation";
            permission = "diagnostic.read";
            request_payload = ReadDtcInformationRequest{message.payload.front() == 1U};
            break;
        case kClearDiagnosticInformationSomeIpMethod:
            if (!message.payload.empty()) {
                return error_response(message, SomeIpReturnCode::MalformedMessage);
            }
            method = "ClearDiagnosticInformation";
            permission = "diagnostic.clear";
            request_payload = ClearDiagnosticInformationRequest{};
            break;
        default:
            return error_response(message, SomeIpReturnCode::UnknownMethod);
    }

    const auto identity = std::to_string(message.client_id) + "-" +
                          std::to_string(message.session_id);
    const ServiceRequest request{"someip-diagnostic-" + identity,
                                 "someip-diagnostic-trace-" + identity,
                                 "someip-diagnostic-client-" +
                                     std::to_string(message.client_id),
                                 "vehicle.diagnostics",
                                 std::move(method),
                                 0,
                                 {std::move(permission)},
                                 std::move(request_payload)};
    const auto response = runtime.invoke(request);
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>(diagnostic_status(response))};
    if (response.accepted &&
        message.method_id == kRequestDiagnosticSeedSomeIpMethod) {
        const auto* seed = std::get_if<DiagnosticSeedResult>(&response.payload);
        if (seed == nullptr) {
            return error_response(message, SomeIpReturnCode::NotOk);
        }
        append_u32(payload, seed->seed);
    } else if (response.accepted &&
               message.method_id == kReadDtcInformationSomeIpMethod) {
        const auto* list = std::get_if<DiagnosticDtcListResult>(&response.payload);
        if (list == nullptr || list->dtcs.size() > 255U) {
            return error_response(message, SomeIpReturnCode::NotOk);
        }
        payload.push_back(static_cast<std::uint8_t>(list->dtcs.size()));
        for (const auto& dtc : list->dtcs) {
            append_u24(payload, dtc.code);
            payload.push_back(dtc.severity);
            payload.push_back(static_cast<std::uint8_t>(
                (dtc.test_failed ? 0x01U : 0U) | (dtc.pending ? 0x02U : 0U) |
                (dtc.confirmed ? 0x04U : 0U)));
            append_u32(payload, dtc.occurrence_count);
        }
    }
    return SomeIpMessage{message.service_id, message.method_id, message.client_id,
                         message.session_id, 1U, SomeIpMessageType::Response,
                         SomeIpReturnCode::Ok, std::move(payload)};
}

}  // namespace

SomeIpMessage make_snapshot_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                           std::int64_t now_ms, std::int64_t max_age_ms) {
    std::vector<std::uint8_t> payload;
    payload.reserve(16U);
    append_i64(payload, now_ms);
    append_i64(payload, max_age_ms);
    return SomeIpMessage{kVehicleStateSomeIpService, kGetSnapshotSomeIpMethod, client_id,
                         session_id, 1U, SomeIpMessageType::Request, SomeIpReturnCode::Ok,
                         std::move(payload)};
}

SomeIpMessage make_door_lock_someip_request(std::uint16_t client_id, std::uint16_t session_id,
                                            std::uint8_t door_mask, bool lock) {
    return SomeIpMessage{kVehicleBodySomeIpService, kSetDoorLocksSomeIpMethod, client_id,
                         session_id, 1U, SomeIpMessageType::Request, SomeIpReturnCode::Ok,
                         {door_mask, static_cast<std::uint8_t>(lock ? 1U : 0U)}};
}

SomeIpMessage make_diagnostic_session_someip_request(std::uint16_t client_id,
                                                     std::uint16_t session_id,
                                                     DiagnosticSession session) {
    return SomeIpMessage{kVehicleDiagnosticSomeIpService,
                         kStartDiagnosticSessionSomeIpMethod, client_id, session_id, 1U,
                         SomeIpMessageType::Request, SomeIpReturnCode::Ok,
                         {static_cast<std::uint8_t>(session == DiagnosticSession::Extended)}};
}

SomeIpMessage make_diagnostic_seed_someip_request(std::uint16_t client_id,
                                                  std::uint16_t session_id) {
    return SomeIpMessage{kVehicleDiagnosticSomeIpService,
                         kRequestDiagnosticSeedSomeIpMethod, client_id, session_id, 1U,
                         SomeIpMessageType::Request, SomeIpReturnCode::Ok, {}};
}

SomeIpMessage make_diagnostic_key_someip_request(std::uint16_t client_id,
                                                 std::uint16_t session_id,
                                                 std::uint32_t seed, std::uint32_t key) {
    std::vector<std::uint8_t> payload;
    append_u32(payload, seed);
    append_u32(payload, key);
    return SomeIpMessage{kVehicleDiagnosticSomeIpService,
                         kSubmitDiagnosticKeySomeIpMethod, client_id, session_id, 1U,
                         SomeIpMessageType::Request, SomeIpReturnCode::Ok,
                         std::move(payload)};
}

SomeIpMessage make_diagnostic_read_someip_request(std::uint16_t client_id,
                                                  std::uint16_t session_id,
                                                  bool active_only) {
    return SomeIpMessage{kVehicleDiagnosticSomeIpService,
                         kReadDtcInformationSomeIpMethod, client_id, session_id, 1U,
                         SomeIpMessageType::Request, SomeIpReturnCode::Ok,
                         {static_cast<std::uint8_t>(active_only)}};
}

SomeIpMessage make_diagnostic_clear_someip_request(std::uint16_t client_id,
                                                   std::uint16_t session_id) {
    return SomeIpMessage{kVehicleDiagnosticSomeIpService,
                         kClearDiagnosticInformationSomeIpMethod, client_id, session_id, 1U,
                         SomeIpMessageType::Request, SomeIpReturnCode::Ok, {}};
}

SomeIpMessage handle_vehicle_someip_request(const SomeIpMessage& message,
                                            VehicleServiceRuntime& runtime) {
    if (message.message_type != SomeIpMessageType::Request) {
        return error_response(message, SomeIpReturnCode::MalformedMessage);
    }
    if (message.service_id != kVehicleStateSomeIpService &&
        message.service_id != kVehicleBodySomeIpService &&
        message.service_id != kVehicleDiagnosticSomeIpService) {
        return error_response(message, SomeIpReturnCode::UnknownService);
    }
    if (message.interface_version != 1U) {
        return error_response(message, SomeIpReturnCode::MalformedMessage);
    }
    if (message.service_id == kVehicleDiagnosticSomeIpService) {
        return handle_diagnostic_request(message, runtime);
    }
    const bool state_request = message.service_id == kVehicleStateSomeIpService;
    const auto expected_method =
        state_request ? kGetSnapshotSomeIpMethod : kSetDoorLocksSomeIpMethod;
    if (message.method_id != expected_method) {
        return error_response(message, SomeIpReturnCode::UnknownMethod);
    }
    const auto expected_payload_size = state_request ? 16U : 2U;
    if (message.interface_version != 1U || message.payload.size() != expected_payload_size ||
        (!state_request && message.payload.at(1U) > 1U)) {
        return error_response(message, SomeIpReturnCode::MalformedMessage);
    }

    const auto request_id = "someip-" + std::to_string(message.client_id) + "-" +
                            std::to_string(message.session_id);
    const auto trace_id = "someip-trace-" + std::to_string(message.client_id) + "-" +
                          std::to_string(message.session_id);
    const ServiceRequest request = state_request
        ? ServiceRequest{request_id,
                         trace_id,
                         "someip-read-client",
                         "vehicle.state",
                         "GetSnapshot",
                         0,
                         {"vehicle.state.read"},
                         GetVehicleSnapshotRequest{read_i64(message.payload, 0U),
                                                   read_i64(message.payload, 8U)}}
        : ServiceRequest{request_id,
                         trace_id,
                         "someip-body-client",
                         "vehicle.body",
                         "SetDoorLocks",
                         0,
                         {"body.write"},
                         SetDoorLocksRequest{message.payload.at(0U),
                                             message.payload.at(1U) == 1U}};
    const auto response = runtime.invoke(request);
    if (!response.accepted) {
        return error_response(message, SomeIpReturnCode::NotOk);
    }

    std::vector<std::uint8_t> payload;
    if (state_request) {
        const auto* snapshot = std::get_if<VehicleSnapshot>(&response.payload);
        if (snapshot == nullptr) {
            return error_response(message, SomeIpReturnCode::NotOk);
        }
        payload.reserve(5U);
        append_u16(payload, static_cast<std::uint16_t>(snapshot->speed_kph * 100.0 + 0.5));
        payload.push_back(static_cast<std::uint8_t>(snapshot->battery_percent));
        payload.push_back(snapshot->door_open ? 1U : 0U);
        const bool all_fresh = !snapshot->vehicle_status.stale &&
                               !snapshot->tire_pressure.stale &&
                               !snapshot->body_status.stale && !snapshot->tbox_health.stale;
        payload.push_back(all_fresh ? 1U : 0U);
    } else {
        const auto* result = std::get_if<DoorLockResult>(&response.payload);
        if (result == nullptr) {
            return error_response(message, SomeIpReturnCode::NotOk);
        }
        payload = {result->requested_mask,
                   static_cast<std::uint8_t>(result->locked ? 1U : 0U)};
    }
    return SomeIpMessage{message.service_id, message.method_id, message.client_id,
                         message.session_id, 1U, SomeIpMessageType::Response,
                         SomeIpReturnCode::Ok, std::move(payload)};
}

bool decode_door_lock_someip_response(const SomeIpMessage& message, DoorLockWireView& output,
                                      std::string& error) {
    if (message.message_type != SomeIpMessageType::Response ||
        message.return_code != SomeIpReturnCode::Ok) {
        error = "SOME/IP door-lock response reports an error";
        return false;
    }
    if (message.service_id != kVehicleBodySomeIpService ||
        message.method_id != kSetDoorLocksSomeIpMethod || message.payload.size() != 2U ||
        message.payload.at(1U) > 1U) {
        error = "SOME/IP door-lock response contract mismatch";
        return false;
    }
    output.requested_mask = message.payload.at(0U);
    output.locked = message.payload.at(1U) == 1U;
    error.clear();
    return true;
}

bool decode_diagnostic_someip_response(const SomeIpMessage& message,
                                       std::uint16_t expected_method,
                                       DiagnosticWireResponse& output, std::string& error) {
    if (expected_method < kStartDiagnosticSessionSomeIpMethod ||
        expected_method > kClearDiagnosticInformationSomeIpMethod) {
        error = "SOME/IP diagnostic decoder received an unknown method contract";
        return false;
    }
    if (message.message_type != SomeIpMessageType::Response ||
        message.return_code != SomeIpReturnCode::Ok ||
        message.service_id != kVehicleDiagnosticSomeIpService ||
        message.method_id != expected_method || message.payload.empty()) {
        error = "SOME/IP diagnostic response contract mismatch";
        return false;
    }
    const auto raw_status = message.payload.front();
    if (raw_status > static_cast<std::uint8_t>(DiagnosticWireStatus::ApplicationError)) {
        error = "SOME/IP diagnostic response has unknown application status";
        return false;
    }
    output = {};
    output.status = static_cast<DiagnosticWireStatus>(raw_status);
    if (output.status != DiagnosticWireStatus::Ok) {
        if (message.payload.size() != 1U) {
            error = "SOME/IP diagnostic error payload has trailing bytes";
            return false;
        }
        error.clear();
        return true;
    }
    if (expected_method == kRequestDiagnosticSeedSomeIpMethod) {
        if (message.payload.size() != 5U) {
            error = "SOME/IP diagnostic seed response length mismatch";
            return false;
        }
        output.seed = read_u32(message.payload, 1U);
    } else if (expected_method == kReadDtcInformationSomeIpMethod) {
        if (message.payload.size() < 2U) {
            error = "SOME/IP diagnostic DTC response length mismatch";
            return false;
        }
        const auto count = static_cast<std::size_t>(message.payload.at(1U));
        if (message.payload.size() != 2U + count * 9U) {
            error = "SOME/IP diagnostic DTC record length mismatch";
            return false;
        }
        output.dtcs.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = 2U + index * 9U;
            const auto flags = message.payload.at(offset + 4U);
            if ((flags & 0xF8U) != 0U) {
                error = "SOME/IP diagnostic DTC flags are malformed";
                return false;
            }
            output.dtcs.push_back(
                {read_u24(message.payload, offset), message.payload.at(offset + 3U),
                 (flags & 0x01U) != 0U, (flags & 0x02U) != 0U,
                 (flags & 0x04U) != 0U, read_u32(message.payload, offset + 5U)});
        }
    } else if (message.payload.size() != 1U) {
        error = "SOME/IP diagnostic acknowledgement length mismatch";
        return false;
    }
    error.clear();
    return true;
}

bool decode_snapshot_someip_response(const SomeIpMessage& message, VehicleSnapshotWireView& output,
                                     std::string& error) {
    if (message.message_type != SomeIpMessageType::Response ||
        message.return_code != SomeIpReturnCode::Ok) {
        error = "SOME/IP response reports an error";
        return false;
    }
    if (message.service_id != kVehicleStateSomeIpService ||
        message.method_id != kGetSnapshotSomeIpMethod || message.payload.size() != 5U) {
        error = "SOME/IP response contract mismatch";
        return false;
    }
    output.speed_kph = static_cast<double>(read_u16(message.payload, 0U)) * 0.01;
    output.battery_percent = static_cast<int>(message.payload.at(2U));
    output.door_open = message.payload.at(3U) == 1U;
    output.all_signal_groups_fresh = message.payload.at(4U) == 1U;
    error.clear();
    return true;
}

}  // namespace sdv

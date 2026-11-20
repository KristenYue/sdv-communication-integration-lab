#include "sdv/vehicle_someip_adapter.hpp"

#include <cstddef>
#include <cstdint>
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

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes.at(offset)) << 8U) |
           static_cast<std::uint16_t>(bytes.at(offset + 1U));
}

SomeIpMessage error_response(const SomeIpMessage& request, SomeIpReturnCode code) {
    return SomeIpMessage{request.service_id, request.method_id, request.client_id,
                         request.session_id, request.interface_version,
                         SomeIpMessageType::Error, code, {}};
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

SomeIpMessage handle_vehicle_someip_request(const SomeIpMessage& message,
                                            VehicleServiceRuntime& runtime) {
    if (message.message_type != SomeIpMessageType::Request) {
        return error_response(message, SomeIpReturnCode::MalformedMessage);
    }
    if (message.service_id != kVehicleStateSomeIpService) {
        return error_response(message, SomeIpReturnCode::UnknownService);
    }
    if (message.method_id != kGetSnapshotSomeIpMethod) {
        return error_response(message, SomeIpReturnCode::UnknownMethod);
    }
    if (message.interface_version != 1U || message.payload.size() != 16U) {
        return error_response(message, SomeIpReturnCode::MalformedMessage);
    }

    const auto now_ms = read_i64(message.payload, 0U);
    const auto max_age_ms = read_i64(message.payload, 8U);
    const auto request_id = "someip-" + std::to_string(message.client_id) + "-" +
                            std::to_string(message.session_id);
    const auto trace_id = "someip-trace-" + std::to_string(message.client_id) + "-" +
                          std::to_string(message.session_id);
    const ServiceRequest request{request_id,
                                 trace_id,
                                 "someip-read-client",
                                 "vehicle.state",
                                 "GetSnapshot",
                                 0,
                                 {"vehicle.state.read"},
                                 GetVehicleSnapshotRequest{now_ms, max_age_ms}};
    const auto response = runtime.invoke(request);
    const auto* snapshot = std::get_if<VehicleSnapshot>(&response.payload);
    if (!response.accepted || snapshot == nullptr) {
        return error_response(message, SomeIpReturnCode::NotOk);
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(5U);
    append_u16(payload, static_cast<std::uint16_t>(snapshot->speed_kph * 100.0 + 0.5));
    payload.push_back(static_cast<std::uint8_t>(snapshot->battery_percent));
    payload.push_back(snapshot->door_open ? 1U : 0U);
    const bool all_fresh = !snapshot->vehicle_status.stale && !snapshot->tire_pressure.stale &&
                           !snapshot->body_status.stale && !snapshot->tbox_health.stale;
    payload.push_back(all_fresh ? 1U : 0U);
    return SomeIpMessage{message.service_id, message.method_id, message.client_id,
                         message.session_id, 1U, SomeIpMessageType::Response,
                         SomeIpReturnCode::Ok, std::move(payload)};
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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdv {

enum class SomeIpMessageType : std::uint8_t {
    Request = 0x00,
    Response = 0x80,
    Error = 0x81,
};

enum class SomeIpReturnCode : std::uint8_t {
    Ok = 0x00,
    NotOk = 0x01,
    UnknownService = 0x02,
    UnknownMethod = 0x03,
    MalformedMessage = 0x09,
};

struct SomeIpMessage {
    std::uint16_t service_id{0};
    std::uint16_t method_id{0};
    std::uint16_t client_id{0};
    std::uint16_t session_id{0};
    std::uint8_t interface_version{1};
    SomeIpMessageType message_type{SomeIpMessageType::Request};
    SomeIpReturnCode return_code{SomeIpReturnCode::Ok};
    std::vector<std::uint8_t> payload;
};

struct SomeIpDecodeResult {
    bool accepted{false};
    std::string code;
    std::string message;
    SomeIpMessage value;
};

std::vector<std::uint8_t> encode_someip(const SomeIpMessage& message);
SomeIpDecodeResult decode_someip(const std::vector<std::uint8_t>& bytes);

}  // namespace sdv

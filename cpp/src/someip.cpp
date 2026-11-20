#include "sdv/someip.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sdv {
namespace {

constexpr std::size_t kHeaderSize = 16U;
constexpr std::uint8_t kProtocolVersion = 1U;
constexpr std::size_t kMaximumPayloadSize = 4'096U;

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes.at(offset)) << 8U) |
           static_cast<std::uint16_t>(bytes.at(offset + 1U));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes.at(offset)) << 24U |
           static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 16U |
           static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 8U |
           static_cast<std::uint32_t>(bytes.at(offset + 3U));
}

bool supported_message_type(std::uint8_t raw) {
    return raw == static_cast<std::uint8_t>(SomeIpMessageType::Request) ||
           raw == static_cast<std::uint8_t>(SomeIpMessageType::Response) ||
           raw == static_cast<std::uint8_t>(SomeIpMessageType::Error);
}

SomeIpDecodeResult reject(std::string code, std::string message) {
    return SomeIpDecodeResult{false, std::move(code), std::move(message), {}};
}

}  // namespace

std::vector<std::uint8_t> encode_someip(const SomeIpMessage& message) {
    if (message.payload.size() > kMaximumPayloadSize ||
        message.payload.size() > std::numeric_limits<std::uint32_t>::max() - 8U) {
        throw std::length_error("SOME/IP payload exceeds project boundary");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize + message.payload.size());
    append_u16(bytes, message.service_id);
    append_u16(bytes, message.method_id);
    append_u32(bytes, static_cast<std::uint32_t>(8U + message.payload.size()));
    append_u16(bytes, message.client_id);
    append_u16(bytes, message.session_id);
    bytes.push_back(kProtocolVersion);
    bytes.push_back(message.interface_version);
    bytes.push_back(static_cast<std::uint8_t>(message.message_type));
    bytes.push_back(static_cast<std::uint8_t>(message.return_code));
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    return bytes;
}

SomeIpDecodeResult decode_someip(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) {
        return reject("TRUNCATED_HEADER", "SOME/IP message requires a 16-byte header");
    }
    const auto covered_length = read_u32(bytes, 4U);
    if (covered_length < 8U) {
        return reject("INVALID_LENGTH", "SOME/IP length must cover the 8-byte tail header");
    }
    if (covered_length > kMaximumPayloadSize + 8U) {
        return reject("PAYLOAD_TOO_LARGE", "SOME/IP payload exceeds project boundary");
    }
    const auto expected_size = static_cast<std::size_t>(covered_length) + 8U;
    if (bytes.size() != expected_size) {
        return reject("LENGTH_MISMATCH", "datagram size does not match SOME/IP length");
    }
    if (bytes.at(12U) != kProtocolVersion) {
        return reject("UNSUPPORTED_PROTOCOL_VERSION", "only SOME/IP protocol version 1 is supported");
    }
    if (!supported_message_type(bytes.at(14U))) {
        return reject("UNSUPPORTED_MESSAGE_TYPE",
                      "project subset supports request, response and error only");
    }

    SomeIpMessage message;
    message.service_id = read_u16(bytes, 0U);
    message.method_id = read_u16(bytes, 2U);
    message.client_id = read_u16(bytes, 8U);
    message.session_id = read_u16(bytes, 10U);
    message.interface_version = bytes.at(13U);
    message.message_type = static_cast<SomeIpMessageType>(bytes.at(14U));
    message.return_code = static_cast<SomeIpReturnCode>(bytes.at(15U));
    message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), bytes.end());
    return SomeIpDecodeResult{true, "OK", "SOME/IP message decoded", std::move(message)};
}

}  // namespace sdv

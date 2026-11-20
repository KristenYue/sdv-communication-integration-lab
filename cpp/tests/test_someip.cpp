#include "sdv/someip.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

void test_known_header_vector() {
    const sdv::SomeIpMessage message{0x1234U,
                                     0x0001U,
                                     0x0042U,
                                     0x0007U,
                                     0x01U,
                                     sdv::SomeIpMessageType::Request,
                                     sdv::SomeIpReturnCode::Ok,
                                     {0xAAU, 0xBBU}};
    const std::vector<std::uint8_t> expected{0x12U, 0x34U, 0x00U, 0x01U, 0x00U, 0x00U,
                                             0x00U, 0x0AU, 0x00U, 0x42U, 0x00U, 0x07U,
                                             0x01U, 0x01U, 0x00U, 0x00U, 0xAAU, 0xBBU};
    const auto encoded = sdv::encode_someip(message);
    check(encoded == expected, "wire bytes match known SOME/IP header vector");
    const auto decoded = sdv::decode_someip(encoded);
    check(decoded.accepted, "known SOME/IP vector decoded");
    check(decoded.value.service_id == 0x1234U && decoded.value.method_id == 0x0001U,
          "message identifier split into service and method");
    check(decoded.value.client_id == 0x0042U && decoded.value.session_id == 0x0007U,
          "request identifier split into client and session");
    check(decoded.value.payload == std::vector<std::uint8_t>({0xAAU, 0xBBU}),
          "payload round trips");
}

void test_malformed_messages_are_rejected() {
    const sdv::SomeIpMessage message{0x1234U, 1U, 2U, 3U, 1U,
                                     sdv::SomeIpMessageType::Request,
                                     sdv::SomeIpReturnCode::Ok, {}};
    const auto valid = sdv::encode_someip(message);

    auto short_header = valid;
    short_header.resize(15U);
    check(sdv::decode_someip(short_header).code == "TRUNCATED_HEADER",
          "truncated header rejected");

    auto bad_length = valid;
    bad_length.at(7U) = 9U;
    check(sdv::decode_someip(bad_length).code == "LENGTH_MISMATCH",
          "mismatched length rejected");

    auto bad_protocol = valid;
    bad_protocol.at(12U) = 2U;
    check(sdv::decode_someip(bad_protocol).code == "UNSUPPORTED_PROTOCOL_VERSION",
          "unknown protocol version rejected");

    auto unsupported_type = valid;
    unsupported_type.at(14U) = 0x02U;
    check(sdv::decode_someip(unsupported_type).code == "UNSUPPORTED_MESSAGE_TYPE",
          "notification outside bounded subset rejected");
}

void test_payload_boundary() {
    sdv::SomeIpMessage message;
    message.payload.resize(4'097U);
    bool threw = false;
    try {
        static_cast<void>(sdv::encode_someip(message));
    } catch (const std::length_error&) {
        threw = true;
    }
    check(threw, "oversized outbound payload rejected");
}

}  // namespace

int main() {
    test_known_header_vector();
    test_malformed_messages_are_rejected();
    test_payload_boundary();
    if (failures == 0) {
        std::cout << "All bounded SOME/IP codec tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}

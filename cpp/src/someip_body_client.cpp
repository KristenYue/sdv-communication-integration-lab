#include "sdv/someip_udp.hpp"
#include "sdv/vehicle_someip_adapter.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 30'491);
    const auto session = static_cast<std::uint16_t>(argc > 2 ? std::stoi(argv[2]) : 1);
    const int timeout_ms = argc > 3 ? std::stoi(argv[3]) : 500;
    const auto mask = static_cast<std::uint8_t>((session % 15U) + 1U);
    const bool lock = session % 2U == 0U;
    const auto request =
        sdv::make_door_lock_someip_request(0x0052U, session, mask, lock);
    const auto exchange = sdv::request_someip_udp(port, request, timeout_ms);
    if (!exchange.accepted) {
        std::cerr << exchange.code << ": " << exchange.message << '\n';
        return 75;
    }
    sdv::DoorLockWireView result;
    std::string error;
    if (!sdv::decode_door_lock_someip_response(exchange.value, result, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "{\"client_id\":82,\"session_id\":" << session
              << ",\"requested_mask\":" << static_cast<unsigned int>(result.requested_mask)
              << ",\"locked\":" << (result.locked ? "true" : "false") << "}\n";
    return EXIT_SUCCESS;
}

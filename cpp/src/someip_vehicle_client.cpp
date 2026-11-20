#include "sdv/someip_udp.hpp"
#include "sdv/vehicle_someip_adapter.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 30'490);
    const auto request = sdv::make_snapshot_someip_request(0x0042U, 0x0007U, 1'050, 100);
    const auto exchange = sdv::request_someip_udp(port, request, 3'000);
    if (!exchange.accepted) {
        std::cerr << exchange.code << ": " << exchange.message << '\n';
        return EXIT_FAILURE;
    }
    sdv::VehicleSnapshotWireView snapshot;
    std::string error;
    if (!sdv::decode_snapshot_someip_response(exchange.value, snapshot, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "{\"service\":\"vehicle.state\",\"method\":\"GetSnapshot\","
              << "\"client_id\":66,\"session_id\":7,\"speed_kph\":"
              << snapshot.speed_kph << ",\"battery_percent\":" << snapshot.battery_percent
              << ",\"door_open\":" << (snapshot.door_open ? "true" : "false")
              << ",\"all_signal_groups_fresh\":"
              << (snapshot.all_signal_groups_fresh ? "true" : "false") << "}\n";
    return EXIT_SUCCESS;
}

#include "sdv/someip_udp.hpp"
#include "sdv/vehicle_gateway.hpp"
#include "sdv/vehicle_service_runtime.hpp"
#include "sdv/vehicle_someip_adapter.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

sdv::CanFrame frame(std::uint32_t id, std::array<std::uint8_t, 8> data,
                    std::int64_t time_ms, std::string trace_id) {
    return sdv::CanFrame{id, 8U, data, time_ms, std::move(trace_id)};
}

bool seed_gateway(sdv::VehicleGateway& gateway) {
    return gateway
               .ingest(frame(sdv::VehicleGateway::kVehicleStatusId,
                             {0x39, 0x30, 75, 0, 0, 0, 0, 0}, 1'000, "seed-vcu"))
               .accepted &&
           gateway
               .ingest(frame(sdv::VehicleGateway::kTirePressureId,
                             {230, 0, 231, 0, 229, 0, 232, 0}, 1'001, "seed-tpms"))
               .accepted &&
           gateway
               .ingest(frame(sdv::VehicleGateway::kBodyStatusId,
                             {0, 0x0F, 0, 0, 0, 0, 0, 0}, 1'002, "seed-bcm"))
               .accepted &&
           gateway
               .ingest(frame(sdv::VehicleGateway::kTboxHealthId,
                             {1, 88, 0, 0, 0, 0, 0, 0}, 1'003, "seed-tbox"))
               .accepted;
}

}  // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 30'490);
    sdv::VehicleGateway gateway;
    if (!seed_gateway(gateway)) {
        std::cerr << "failed to seed vehicle gateway\n";
        return EXIT_FAILURE;
    }
    auto clock = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    sdv::VehicleServiceRuntime runtime(clock);
    if (!sdv::register_vehicle_state_service(runtime, gateway)) {
        std::cerr << "failed to register vehicle state service\n";
        return EXIT_FAILURE;
    }
    sdv::SomeIpUdpServer server(port, 5'000);
    if (!server.valid()) {
        std::cerr << server.error() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "READY port=" << port << std::endl;
    const auto result = server.serve_once([&runtime](const sdv::SomeIpMessage& request) {
        return sdv::handle_vehicle_someip_request(request, runtime);
    });
    if (!result.accepted) {
        std::cerr << result.code << ": " << result.message << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "SERVED runtime_trace_events=" << runtime.trace().size() << '\n';
    return EXIT_SUCCESS;
}

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

bool seed_diagnostics(sdv::DiagnosticManager& diagnostics) {
    constexpr int kDtcCount = 10;
    for (int index = 0; index < kDtcCount; ++index) {
        if (!diagnostics.register_dtc(0xC40001U + static_cast<std::uint32_t>(index),
                                      "REMOTE_DTC_" + std::to_string(index + 1), 2U)) {
            return false;
        }
    }
    for (std::uint64_t cycle = 1U; cycle <= 2U; ++cycle) {
        if (!diagnostics
                 .begin_operation_cycle(cycle, static_cast<std::int64_t>(cycle * 100U),
                                        "remote-cycle")
                 .accepted) {
            return false;
        }
        for (int index = 0; index < kDtcCount; ++index) {
            if (!diagnostics
                     .report_test_result(
                         0xC40001U + static_cast<std::uint32_t>(index), true,
                         sdv::VehicleSnapshot{},
                         static_cast<std::int64_t>(cycle * 100U + index + 1U),
                         "remote-failure")
                     .accepted) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 30'490);
    const std::string mode = argc > 2 ? argv[2] : "snapshot";
    const std::string journal = argc > 3 ? argv[3] : "";
    const int request_count = argc > 4 ? std::stoi(argv[4]) : 1;
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
    sdv::VehicleServiceRuntime runtime(clock, journal);
    if (!sdv::register_vehicle_state_service(runtime, gateway)) {
        std::cerr << "failed to register vehicle state service\n";
        return EXIT_FAILURE;
    }
    int body_handler_executions = 0;
    sdv::DiagnosticManager diagnostics;
    if (mode == "body-drop" || mode == "body-replay") {
        const sdv::ServiceDescriptor body{"vehicle.body", "1.0.0",
                                          {{"SetDoorLocks", "body.write", true}}};
        if (!runtime.register_service(
                body, [&body_handler_executions](const sdv::ServiceRequest& request) {
                    ++body_handler_executions;
                    const auto& command =
                        std::get<sdv::SetDoorLocksRequest>(request.payload);
                    return sdv::ServiceResponse{
                        true, "OK", "door lock command applied",
                        sdv::DoorLockResult{command.door_mask, command.lock}};
                })) {
            std::cerr << "failed to register vehicle body service\n";
            return EXIT_FAILURE;
        }
    }
    if (mode == "diagnostic") {
        if (!seed_diagnostics(diagnostics) ||
            !sdv::register_vehicle_diagnostic_service(runtime, diagnostics, clock)) {
            std::cerr << "failed to initialize vehicle diagnostic service\n";
            return EXIT_FAILURE;
        }
    }
    sdv::SomeIpUdpServer server(port, 5'000);
    if (!server.valid()) {
        std::cerr << server.error() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "READY port=" << port << " mode=" << mode << " requests=" << request_count
              << std::endl;
    for (int index = 0; index < request_count; ++index) {
        const auto result = server.serve_once(
            [&runtime](const sdv::SomeIpMessage& request) {
                return sdv::handle_vehicle_someip_request(request, runtime);
            },
            mode != "body-drop");
        if (!result.accepted) {
            std::cerr << result.code << ": " << result.message << '\n';
            return EXIT_FAILURE;
        }
    }
    std::cout << "SERVED requests=" << request_count
              << " body_handler_executions=" << body_handler_executions
              << " runtime_trace_events=" << runtime.trace_snapshot().size()
              << " diagnostic_trace_events=" << diagnostics.trace_snapshot().size()
              << " active_dtcs=" << diagnostics.active_dtcs().size() << '\n';
    return EXIT_SUCCESS;
}

#include "sdv/vehicle_service_runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

sdv::ServiceDescriptor body_service() {
    return sdv::ServiceDescriptor{"vehicle.body", "1.0.0",
                                  {{"SetDoorLocks", "body.write", true}}};
}

sdv::ServiceRequest command(int index) {
    return sdv::ServiceRequest{"restart-command-" + std::to_string(index),
                               "restart-trace-" + std::to_string(index),
                               "body-controller",
                               "vehicle.body",
                               "SetDoorLocks",
                               10'000,
                               {"body.write"},
                               sdv::SetDoorLocksRequest{
                                   static_cast<std::uint8_t>((index % 15) + 1),
                                   index % 2 == 0}};
}

bool register_handler(sdv::VehicleServiceRuntime& runtime, int& executions, bool crash) {
    return runtime.register_service(
        body_service(), [&executions, crash](const sdv::ServiceRequest& request) {
            ++executions;
            if (crash) {
                std::_Exit(73);
            }
            const auto& value = std::get<sdv::SetDoorLocksRequest>(request.payload);
            return sdv::ServiceResponse{true, "OK", "door lock command applied",
                                        sdv::DoorLockResult{value.door_mask, value.lock}};
        });
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "usage: restart_recovery_evidence MODE JOURNAL\n";
            return EXIT_FAILURE;
        }
        const std::string mode = argv[1];
        const std::string journal = argv[2];
        std::int64_t now = 1'000;
        int executions = 0;
        sdv::VehicleServiceRuntime runtime([&now] { return now; }, journal);
        if (!register_handler(runtime, executions, mode == "crash")) {
            return EXIT_FAILURE;
        }

        if (mode == "first" || mode == "replay") {
            int accepted = 0;
            int conflicts = 0;
            constexpr int kGroups = 60;
            for (int index = 0; index < kGroups; ++index) {
                auto request = command(index);
                if (runtime.invoke(request).accepted) {
                    ++accepted;
                }
                if (mode == "replay") {
                    request.payload = sdv::SetDoorLocksRequest{0x0FU, index % 2 != 0};
                    if (runtime.invoke(request).code == "REQUEST_ID_CONFLICT") {
                        ++conflicts;
                    }
                }
            }
            std::cout << "{\"mode\":\"" << mode << "\",\"groups\":" << kGroups
                      << ",\"accepted\":" << accepted << ",\"handler_executions\":"
                      << executions << ",\"conflicts_rejected\":" << conflicts << "}\n";
            const bool passed = accepted == kGroups &&
                                ((mode == "first" && executions == kGroups && conflicts == 0) ||
                                 (mode == "replay" && executions == 0 && conflicts == kGroups));
            return passed ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        if (mode == "crash") {
            static_cast<void>(runtime.invoke(command(999)));
            return EXIT_FAILURE;
        }

        if (mode == "recover-unknown") {
            const auto response = runtime.invoke(command(999));
            std::cout << "{\"mode\":\"recover-unknown\",\"code\":\"" << response.code
                      << "\",\"handler_executions\":" << executions << "}\n";
            return response.code == "COMPLETION_UNKNOWN" && executions == 0 ? EXIT_SUCCESS
                                                                             : EXIT_FAILURE;
        }

        std::cerr << "unknown mode\n";
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "recovery startup failed: " << error.what() << '\n';
        return 74;
    }
}

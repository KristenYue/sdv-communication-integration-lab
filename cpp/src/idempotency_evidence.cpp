#include "sdv/vehicle_service_runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    std::int64_t now = 1'000;
    int handler_executions = 0;
    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    const sdv::ServiceDescriptor body{"vehicle.body", "1.0.0",
                                      {{"SetDoorLocks", "body.write"}}};
    if (!runtime.register_service(
            body, [&handler_executions](const sdv::ServiceRequest& request) {
                ++handler_executions;
                const auto& command = std::get<sdv::SetDoorLocksRequest>(request.payload);
                return sdv::ServiceResponse{
                    true, "OK", "door lock command applied",
                    sdv::DoorLockResult{command.door_mask, command.lock}};
            })) {
        return EXIT_FAILURE;
    }

    int first_successes = 0;
    int replay_successes = 0;
    int conflicts_rejected = 0;
    constexpr int kGroups = 60;
    for (int index = 0; index < kGroups; ++index) {
        const auto mask = static_cast<std::uint8_t>((index % 15) + 1);
        const bool lock = index % 2 == 0;
        const auto request_id = "body-command-" + std::to_string(index);
        sdv::ServiceRequest request{request_id,
                                    "trace-first-" + std::to_string(index),
                                    "body-controller",
                                    "vehicle.body",
                                    "SetDoorLocks",
                                    2'000,
                                    {"body.write"},
                                    sdv::SetDoorLocksRequest{mask, lock}};
        if (runtime.invoke(request).accepted) {
            ++first_successes;
        }
        request.trace_id = "trace-retry-" + std::to_string(index);
        request.deadline_ms = 3'000;
        if (runtime.invoke(request).accepted) {
            ++replay_successes;
        }
        request.trace_id = "trace-conflict-" + std::to_string(index);
        request.payload = sdv::SetDoorLocksRequest{mask, !lock};
        if (runtime.invoke(request).code == "REQUEST_ID_CONFLICT") {
            ++conflicts_rejected;
        }
    }

    int replay_events = 0;
    for (const auto& event : runtime.trace()) {
        if (event.event == "IDEMPOTENT_REPLAY") {
            ++replay_events;
        }
    }
    const int duplicate_final_actions = handler_executions - kGroups;
    std::cout << "{\"groups\":" << kGroups << ",\"total_invocations\":" << kGroups * 3
              << ",\"first_successes\":" << first_successes
              << ",\"replay_successes\":" << replay_successes
              << ",\"conflicts_rejected\":" << conflicts_rejected
              << ",\"handler_executions\":" << handler_executions
              << ",\"duplicate_final_actions\":" << duplicate_final_actions
              << ",\"idempotent_replay_events\":" << replay_events << "}\n";

    const bool passed = first_successes == kGroups && replay_successes == kGroups &&
                        conflicts_rejected == kGroups && handler_executions == kGroups &&
                        duplicate_final_actions == 0 && replay_events == kGroups;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

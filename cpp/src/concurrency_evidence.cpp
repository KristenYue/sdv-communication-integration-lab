#include "sdv/vehicle_service_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

sdv::ServiceDescriptor body_service() {
    return sdv::ServiceDescriptor{"vehicle.body", "1.0.0",
                                  {{"SetDoorLocks", "body.write", true}}};
}

sdv::ServiceRequest request(std::string id, std::string trace, std::uint8_t mask, bool lock,
                            std::int64_t deadline = 0) {
    return sdv::ServiceRequest{std::move(id),
                               std::move(trace),
                               "body-controller",
                               "vehicle.body",
                               "SetDoorLocks",
                               deadline,
                               {"body.write"},
                               sdv::SetDoorLocksRequest{mask, lock}};
}

sdv::ServiceResponse success(const sdv::ServiceRequest& value) {
    const auto& command = std::get<sdv::SetDoorLocksRequest>(value.payload);
    return sdv::ServiceResponse{true, "OK", "door lock command applied",
                                sdv::DoorLockResult{command.door_mask, command.lock}};
}

}  // namespace

int main() {
    std::atomic<int> executions{0};
    std::atomic<bool> release_identical{false};
    std::atomic<bool> conflict_handler_started{false};
    sdv::VehicleServiceRuntime runtime(now_ms);
    if (!runtime.register_service(body_service(), [&](const sdv::ServiceRequest& value) {
            ++executions;
            if (value.request_id == "conflict-command") {
                conflict_handler_started = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            } else if (value.request_id == "same-command") {
                while (!release_identical.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
            return success(value);
        })) {
        return EXIT_FAILURE;
    }

    constexpr int kIdenticalThreads = 32;
    std::atomic<int> identical_successes{0};
    std::vector<std::thread> identical_threads;
    identical_threads.reserve(kIdenticalThreads);
    for (int index = 0; index < kIdenticalThreads; ++index) {
        identical_threads.emplace_back([&, index] {
            const auto response = runtime.invoke(request(
                "same-command", "same-trace-" + std::to_string(index), 0x0FU, true,
                now_ms() + 5'000));
            if (response.accepted) {
                ++identical_successes;
            }
        });
    }
    const auto identical_wait_deadline = now_ms() + 5'000;
    int observed_waiters = 0;
    while (observed_waiters < kIdenticalThreads - 1 && now_ms() < identical_wait_deadline) {
        observed_waiters = 0;
        for (const auto& event : runtime.trace_snapshot()) {
            if (event.event == "IN_FLIGHT_WAIT") {
                ++observed_waiters;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    release_identical = true;
    for (auto& thread : identical_threads) {
        thread.join();
    }
    const int after_identical_executions = executions.load();

    auto leader_request = request("conflict-command", "conflict-leader", 0x03U, true,
                                  now_ms() + 5'000);
    sdv::ServiceResponse leader_response;
    std::thread leader([&] { leader_response = runtime.invoke(leader_request); });
    while (!conflict_handler_started.load()) {
        std::this_thread::yield();
    }
    constexpr int kConflictThreads = 16;
    std::atomic<int> conflicts_rejected{0};
    std::vector<std::thread> conflict_threads;
    conflict_threads.reserve(kConflictThreads);
    for (int index = 0; index < kConflictThreads; ++index) {
        conflict_threads.emplace_back([&, index] {
            const auto response = runtime.invoke(request(
                "conflict-command", "conflict-trace-" + std::to_string(index), 0x0CU, false,
                now_ms() + 5'000));
            if (response.code == "REQUEST_ID_CONFLICT") {
                ++conflicts_rejected;
            }
        });
    }
    for (auto& thread : conflict_threads) {
        thread.join();
    }
    leader.join();

    std::atomic<int> capacity_started{0};
    std::atomic<bool> release_capacity{false};
    sdv::VehicleServiceRuntime capacity_runtime(now_ms, {}, 2U);
    if (!capacity_runtime.register_service(body_service(), [&](const sdv::ServiceRequest& value) {
            ++capacity_started;
            while (!release_capacity.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return success(value);
        })) {
        return EXIT_FAILURE;
    }
    std::thread first_capacity([&] {
        static_cast<void>(capacity_runtime.invoke(
            request("capacity-1", "capacity-trace-1", 1U, true)));
    });
    std::thread second_capacity([&] {
        static_cast<void>(capacity_runtime.invoke(
            request("capacity-2", "capacity-trace-2", 2U, true)));
    });
    while (capacity_started.load() < 2) {
        std::this_thread::yield();
    }
    const auto no_deadline = capacity_runtime.invoke(
        request("capacity-4", "capacity-trace-4", 4U, true));
    const auto queued = capacity_runtime.invoke(
        request("capacity-3", "capacity-trace-3", 3U, true, now_ms() + 50));
    release_capacity = true;
    first_capacity.join();
    second_capacity.join();

    int replay_events = 0;
    int in_flight_wait_events = 0;
    for (const auto& event : runtime.trace_snapshot()) {
        if (event.event == "IDEMPOTENT_REPLAY") {
            ++replay_events;
        } else if (event.event == "IN_FLIGHT_WAIT") {
            ++in_flight_wait_events;
        }
    }

    std::cout << "{\"identical_threads\":" << kIdenticalThreads
              << ",\"identical_successes\":" << identical_successes.load()
              << ",\"identical_handler_executions\":" << after_identical_executions
              << ",\"idempotent_replay_events\":" << replay_events
              << ",\"in_flight_wait_events\":" << in_flight_wait_events
              << ",\"conflict_threads\":" << kConflictThreads
              << ",\"conflicts_rejected\":" << conflicts_rejected.load()
              << ",\"conflict_leader_accepted\":"
              << (leader_response.accepted ? "true" : "false")
              << ",\"capacity_limit\":2,\"capacity_started\":" << capacity_started.load()
              << ",\"no_deadline_result\":\"" << no_deadline.code << "\""
              << ",\"queued_result\":\"" << queued.code << "\"}\n";

    const bool passed = identical_successes == kIdenticalThreads &&
                        after_identical_executions == 1 &&
                        replay_events == kIdenticalThreads - 1 &&
                        in_flight_wait_events >= kIdenticalThreads - 1 &&
                        conflicts_rejected == kConflictThreads && leader_response.accepted &&
                        executions == 2 && capacity_started == 2 &&
                        no_deadline.code == "QUEUE_FULL" &&
                        queued.code == "QUEUE_TIMEOUT";
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

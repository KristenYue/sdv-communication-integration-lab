#include "sdv/vehicle_service_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

std::int64_t clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::size_t resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0U;
    }
    return static_cast<std::size_t>(counters.WorkingSetSize);
#else
    std::ifstream input("/proc/self/statm");
    std::size_t total_pages = 0;
    std::size_t resident_pages = 0;
    input >> total_pages >> resident_pages;
    static_cast<void>(total_pages);
    const long page_size = sysconf(_SC_PAGESIZE);
    return input && page_size > 0
               ? resident_pages * static_cast<std::size_t>(page_size)
               : 0U;
#endif
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
        return 0.0;
    }
    ULARGE_INTEGER kernel_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER user_ticks{};
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    constexpr double kTicksPerSecond = 10'000'000.0;
    return static_cast<double>(kernel_ticks.QuadPart + user_ticks.QuadPart) /
           kTicksPerSecond;
#else
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
#endif
}

double percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(
        std::ceil(quantile * static_cast<double>(sorted.size())));
    return sorted.at(std::min(sorted.size() - 1U, std::max<std::size_t>(1U, rank) - 1U));
}

struct CellResult {
    int frequency_hz{0};
    std::size_t payload_bytes{0};
    int nodes{0};
    int attempted{0};
    int succeeded{0};
    double elapsed_seconds{0.0};
    double throughput_hz{0.0};
    double latency_p50_us{0.0};
    double latency_p95_us{0.0};
    double latency_p99_us{0.0};
    double cpu_percent{0.0};
    double rss_mib{0.0};
};

CellResult run_cell(int frequency_hz, std::size_t payload_bytes, int nodes) {
    sdv::VehicleServiceRuntime runtime(clock_ms, {}, static_cast<std::size_t>(nodes));
    const sdv::ServiceDescriptor telemetry{
        "vehicle.telemetry", "1.0.0", {{"Upload", "telemetry.write", false}}};
    if (!runtime.register_service(
            telemetry, [](const sdv::ServiceRequest& request) {
                const auto* upload = std::get_if<sdv::TelemetryUploadRequest>(&request.payload);
                if (upload == nullptr || upload->payload.empty()) {
                    return sdv::ServiceResponse{false, "INVALID_PAYLOAD",
                                                "telemetry payload must not be empty",
                                                std::monostate{}};
                }
                return sdv::ServiceResponse{true, "OK", "telemetry accepted",
                                            sdv::TelemetryUploadResult{upload->payload.size()}};
            })) {
        throw std::runtime_error("telemetry service registration failed");
    }

    constexpr double kDurationSeconds = 0.25;
    const int attempted = static_cast<int>(frequency_hz * kDurationSeconds);
    std::atomic<int> succeeded{0};
    std::vector<double> latencies_us;
    latencies_us.reserve(static_cast<std::size_t>(attempted));
    std::mutex latency_mutex;
    const auto scheduled_start = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(20);
    const double cpu_start = process_cpu_seconds();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(nodes));
    for (int node = 0; node < nodes; ++node) {
        workers.emplace_back([&, node] {
            std::vector<std::uint8_t> payload(payload_bytes);
            for (std::size_t index = 0; index < payload.size(); ++index) {
                payload.at(index) = static_cast<std::uint8_t>((index + node) & 0xFFU);
            }
            for (int sequence = node; sequence < attempted; sequence += nodes) {
                const auto offset_us = static_cast<std::int64_t>(
                    static_cast<double>(sequence) * 1'000'000.0 /
                    static_cast<double>(frequency_hz));
                std::this_thread::sleep_until(scheduled_start +
                                              std::chrono::microseconds(offset_us));
                const sdv::ServiceRequest request{
                    "perf-" + std::to_string(frequency_hz) + '-' +
                        std::to_string(payload_bytes) + '-' + std::to_string(nodes) + '-' +
                        std::to_string(sequence),
                    "perf-trace-" + std::to_string(sequence),
                    "telemetry-node-" + std::to_string(node),
                    "vehicle.telemetry",
                    "Upload",
                    0,
                    {"telemetry.write"},
                    sdv::TelemetryUploadRequest{static_cast<std::uint32_t>(node), payload}};
                const auto begin = std::chrono::steady_clock::now();
                const auto response = runtime.invoke(request);
                const auto end = std::chrono::steady_clock::now();
                const auto* result = std::get_if<sdv::TelemetryUploadResult>(&response.payload);
                if (response.accepted && result != nullptr &&
                    result->accepted_bytes == payload_bytes) {
                    ++succeeded;
                }
                const auto latency =
                    std::chrono::duration<double, std::micro>(end - begin).count();
                std::lock_guard lock(latency_mutex);
                latencies_us.push_back(latency);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();
    const double elapsed =
        std::chrono::duration<double>(wall_end - scheduled_start).count();
    const double cpu_seconds = cpu_end - cpu_start;
    std::sort(latencies_us.begin(), latencies_us.end());
    constexpr double kBytesPerMib = 1024.0 * 1024.0;
    return CellResult{frequency_hz,
                      payload_bytes,
                      nodes,
                      attempted,
                      succeeded.load(),
                      elapsed,
                      static_cast<double>(succeeded.load()) / elapsed,
                      percentile(latencies_us, 0.50),
                      percentile(latencies_us, 0.95),
                      percentile(latencies_us, 0.99),
                      cpu_seconds / elapsed * 100.0,
                      static_cast<double>(resident_bytes()) / kBytesPerMib};
}

}  // namespace

int main() {
    const std::vector<int> frequencies{100, 500, 1'000};
    const std::vector<std::size_t> payloads{64U, 256U, 1'024U};
    const std::vector<int> node_counts{1, 4, 8};
    std::vector<CellResult> results;
    for (const int frequency : frequencies) {
        for (const auto payload : payloads) {
            for (const int nodes : node_counts) {
                results.push_back(run_cell(frequency, payload, nodes));
            }
        }
    }

    std::cout << std::fixed << std::setprecision(4)
              << "{\"schema_version\":\"sdv-runtime-performance-matrix-v1\","
              << "\"data_boundary\":\"in_process_cpp_runtime_not_network_latency\","
              << "\"duration_seconds_per_cell\":0.25,\"cells\":[";
    bool all_passed = true;
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& cell = results.at(index);
        if (index > 0U) {
            std::cout << ',';
        }
        std::cout << "{\"frequency_hz\":" << cell.frequency_hz
                  << ",\"payload_bytes\":" << cell.payload_bytes
                  << ",\"nodes\":" << cell.nodes << ",\"attempted\":" << cell.attempted
                  << ",\"succeeded\":" << cell.succeeded
                  << ",\"elapsed_seconds\":" << cell.elapsed_seconds
                  << ",\"throughput_hz\":" << cell.throughput_hz
                  << ",\"latency_p50_us\":" << cell.latency_p50_us
                  << ",\"latency_p95_us\":" << cell.latency_p95_us
                  << ",\"latency_p99_us\":" << cell.latency_p99_us
                  << ",\"cpu_percent\":" << cell.cpu_percent
                  << ",\"rss_mib\":" << cell.rss_mib << '}';
        all_passed = all_passed && cell.succeeded == cell.attempted;
    }
    std::cout << "]}\n";
    return all_passed && results.size() == 27U ? 0 : 1;
}

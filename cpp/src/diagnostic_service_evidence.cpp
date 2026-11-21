#include "sdv/vehicle_service_runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace {

sdv::ServiceRequest request(std::string id, std::string trace, std::string method,
                            std::set<std::string> permissions,
                            sdv::ServiceRequestPayload payload) {
    return {std::move(id),       std::move(trace), "evidence-diagnostic-tool",
            "vehicle.diagnostics", std::move(method), 2'000,
            std::move(permissions), std::move(payload)};
}

}  // namespace

int main() {
    std::int64_t now = 1'000;
    sdv::DiagnosticManager diagnostics;
    constexpr int kDtcCount = 10;
    for (int index = 0; index < kDtcCount; ++index) {
        const auto code = 0xC20000U + static_cast<std::uint32_t>(index + 1);
        if (!diagnostics.register_dtc(code, "SERVICE_DTC_" + std::to_string(index + 1), 2U)) {
            return EXIT_FAILURE;
        }
    }
    for (std::uint64_t cycle = 1U; cycle <= 2U; ++cycle) {
        if (!diagnostics
                 .begin_operation_cycle(cycle, static_cast<std::int64_t>(cycle * 100U),
                                        "service-cycle")
                 .accepted) {
            return EXIT_FAILURE;
        }
        for (int index = 0; index < kDtcCount; ++index) {
            const auto code = 0xC20000U + static_cast<std::uint32_t>(index + 1);
            if (!diagnostics
                     .report_test_result(code, true, sdv::VehicleSnapshot{},
                                         static_cast<std::int64_t>(cycle * 100U + index + 1U),
                                         "service-failure")
                     .accepted) {
                return EXIT_FAILURE;
            }
        }
    }

    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    if (!sdv::register_vehicle_diagnostic_service(runtime, diagnostics,
                                                  [&now] { return now; })) {
        return EXIT_FAILURE;
    }
    const auto discovered = runtime.discover();
    const int discovered_methods =
        discovered.empty() ? 0 : static_cast<int>(discovered.front().methods.size());

    int permission_rejections = 0;
    for (int index = 0; index < 50; ++index) {
        const auto response = runtime.invoke(request(
            "unauthorized-read-" + std::to_string(index),
            "unauthorized-trace-" + std::to_string(index), "ReadDTCInformation", {},
            sdv::ReadDtcInformationRequest{true}));
        permission_rejections += response.code == "PERMISSION_DENIED";
    }

    const auto read = runtime.invoke(request("authorized-read", "read-trace",
                                             "ReadDTCInformation", {"diagnostic.read"},
                                             sdv::ReadDtcInformationRequest{true}));
    const auto* initial_list = std::get_if<sdv::DiagnosticDtcListResult>(&read.payload);
    const int dtcs_read = initial_list == nullptr ? -1 : static_cast<int>(initial_list->dtcs.size());

    const auto clear_denied = runtime.invoke(request(
        "clear-denied", "clear-denied-trace", "ClearDiagnosticInformation",
        {"diagnostic.clear"}, sdv::ClearDiagnosticInformationRequest{}));

    const auto session = runtime.invoke(request(
        "start-session", "session-trace", "StartSession", {"diagnostic.session"},
        sdv::StartDiagnosticSessionRequest{sdv::DiagnosticSession::Extended}));
    const auto seed_response = runtime.invoke(request(
        "request-seed", "seed-trace", "RequestSecuritySeed", {"diagnostic.security"},
        sdv::RequestDiagnosticSeedRequest{}));
    const auto* seed = std::get_if<sdv::DiagnosticSeedResult>(&seed_response.payload);
    if (!session.accepted || seed == nullptr) {
        return EXIT_FAILURE;
    }
    const auto key = runtime.invoke(request(
        "submit-key", "key-trace", "SubmitSecurityKey", {"diagnostic.security"},
        sdv::SubmitDiagnosticKeyRequest{seed->seed,
                                        sdv::DiagnosticManager::calculate_test_key(seed->seed)}));
    auto clear_request = request("clear-authorized", "clear-trace",
                                 "ClearDiagnosticInformation", {"diagnostic.clear"},
                                 sdv::ClearDiagnosticInformationRequest{});
    const auto cleared = runtime.invoke(clear_request);
    clear_request.trace_id = "clear-retry-trace";
    const auto clear_replay = runtime.invoke(clear_request);
    const auto post_read = runtime.invoke(request(
        "post-clear-read", "post-clear-trace", "ReadDTCInformation", {"diagnostic.read"},
        sdv::ReadDtcInformationRequest{true}));
    const auto* post_list = std::get_if<sdv::DiagnosticDtcListResult>(&post_read.payload);
    const int active_after_clear =
        post_list == nullptr ? -1 : static_cast<int>(post_list->dtcs.size());

    int idempotent_replays = 0;
    for (const auto& event : runtime.trace_snapshot()) {
        idempotent_replays += event.event == "IDEMPOTENT_REPLAY";
    }
    const auto runtime_trace_events = runtime.trace_snapshot().size();
    const auto diagnostic_trace_events = diagnostics.trace_snapshot().size();

    std::cout << "{\"discovered_methods\":" << discovered_methods
              << ",\"permission_rejections\":" << permission_rejections
              << ",\"dtcs_read\":" << dtcs_read << ",\"default_clear_code\":\""
              << clear_denied.code << "\",\"session_accepted\":"
              << (session.accepted ? "true" : "false") << ",\"seed_returned\":"
              << (seed_response.accepted ? "true" : "false") << ",\"key_accepted\":"
              << (key.accepted ? "true" : "false") << ",\"clear_accepted\":"
              << (cleared.accepted ? "true" : "false") << ",\"clear_replay_accepted\":"
              << (clear_replay.accepted ? "true" : "false")
              << ",\"idempotent_replays\":" << idempotent_replays
              << ",\"active_after_clear\":" << active_after_clear
              << ",\"runtime_trace_events\":" << runtime_trace_events
              << ",\"diagnostic_trace_events\":" << diagnostic_trace_events << "}\n";

    const bool passed = discovered_methods == 5 && permission_rejections == 50 &&
                        dtcs_read == kDtcCount && clear_denied.code == "SESSION_DENIED" &&
                        session.accepted && seed_response.accepted && key.accepted &&
                        cleared.accepted && clear_replay.accepted && idempotent_replays == 1 &&
                        active_after_clear == 0;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

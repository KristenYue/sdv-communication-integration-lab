#include "sdv/diagnostic_manager.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

sdv::VehicleSnapshot snapshot(int index) {
    sdv::VehicleSnapshot value;
    value.speed_kph = 60.0 + static_cast<double>(index) * 0.5;
    value.battery_percent = 80 - index;
    value.tire_kpa = {230 - index, 231 - index, 229 - index, 232 - index};
    value.cloud_connected = true;
    value.signal_quality_percent = 90 - index;
    return value;
}

bool unlock(sdv::DiagnosticManager& manager, const std::string& client,
            std::int64_t now_ms) {
    if (!manager
             .start_session(client, sdv::DiagnosticSession::Extended, now_ms,
                            "evidence-session")
             .accepted) {
        return false;
    }
    const auto seed = manager.request_security_seed(client, now_ms + 1, "evidence-seed");
    return seed.accepted &&
           manager
               .submit_security_key(
                   client, seed.seed, sdv::DiagnosticManager::calculate_test_key(seed.seed),
                   now_ms + 2, "evidence-key")
               .accepted;
}

void register_dtcs(sdv::DiagnosticManager& manager, int count) {
    for (int index = 0; index < count; ++index) {
        if (!manager.register_dtc(0xC10000U + static_cast<std::uint32_t>(index + 1),
                                  "EVIDENCE_DTC_" + std::to_string(index + 1),
                                  static_cast<std::uint8_t>(index % 4))) {
            throw std::runtime_error("could not register evidence DTC");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sdv_diagnostic_evidence <journal-path>\n";
        return EXIT_FAILURE;
    }
    const std::filesystem::path journal_path = argv[1];
    std::error_code remove_error;
    std::filesystem::remove(journal_path, remove_error);
    constexpr int kDtcCount = 20;
    int failure_reports = 0;
    int same_cycle_replays = 0;
    int unauthorized_clears = 0;
    int security_lockouts = 0;
    int confirmed_before_restart = 0;
    int freeze_frames_before_restart = 0;
    int recovered_confirmed = 0;
    int active_after_clear_restart = -1;
    bool authorized_clear = false;
    bool corruption_rejected = false;
    std::size_t trace_events = 0U;

    {
        sdv::DiagnosticManager manager(journal_path);
        register_dtcs(manager, kDtcCount);
        if (!manager.begin_operation_cycle(1U, 1'000, "evidence-cycle-1").accepted) {
            return EXIT_FAILURE;
        }
        for (int index = 0; index < kDtcCount; ++index) {
            const auto code = 0xC10000U + static_cast<std::uint32_t>(index + 1);
            failure_reports += manager
                                   .report_test_result(code, true, snapshot(index), 1'010 + index,
                                                       "evidence-first-failure")
                                   .accepted;
            same_cycle_replays += manager
                                      .report_test_result(code, true, snapshot(index + 1),
                                                          1'040 + index,
                                                          "evidence-same-cycle-repeat")
                                      .accepted;
        }
        if (!manager.begin_operation_cycle(2U, 2'000, "evidence-cycle-2").accepted) {
            return EXIT_FAILURE;
        }
        for (int index = 0; index < kDtcCount; ++index) {
            const auto code = 0xC10000U + static_cast<std::uint32_t>(index + 1);
            failure_reports += manager
                                   .report_test_result(code, true, snapshot(index), 2'010 + index,
                                                       "evidence-second-failure")
                                   .accepted;
        }
        for (const auto& dtc : manager.active_dtcs()) {
            confirmed_before_restart += dtc.confirmed;
            freeze_frames_before_restart += dtc.freeze_frame.valid;
            if (dtc.occurrence_count != 2U) {
                return EXIT_FAILURE;
            }
        }
        trace_events += manager.trace_snapshot().size();
    }

    {
        sdv::DiagnosticManager recovered(journal_path);
        register_dtcs(recovered, kDtcCount);
        for (const auto& dtc : recovered.active_dtcs()) {
            recovered_confirmed += dtc.confirmed;
        }
        for (int index = 0; index < 30; ++index) {
            unauthorized_clears +=
                recovered.clear_all("unauthorized-tool", 3'000 + index,
                                    "evidence-unauthorized-clear")
                        .code == "SESSION_DENIED";
        }
        for (int index = 0; index < kDtcCount; ++index) {
            const auto client = "bad-key-client-" + std::to_string(index);
            if (!recovered
                     .start_session(client, sdv::DiagnosticSession::Extended, 4'000,
                                    "evidence-bad-session")
                     .accepted) {
                return EXIT_FAILURE;
            }
            const auto seed =
                recovered.request_security_seed(client, 4'001, "evidence-bad-seed");
            if (!seed.accepted) {
                return EXIT_FAILURE;
            }
            static_cast<void>(recovered.submit_security_key(client, seed.seed, 0U, 4'002,
                                                             "evidence-bad-key-1"));
            static_cast<void>(recovered.submit_security_key(client, seed.seed, 0U, 4'003,
                                                             "evidence-bad-key-2"));
            security_lockouts +=
                recovered
                    .submit_security_key(client, seed.seed, 0U, 4'004,
                                         "evidence-bad-key-3")
                    .code == "SECURITY_LOCKED";
        }
        if (!unlock(recovered, "authorized-tool", 5'000)) {
            return EXIT_FAILURE;
        }
        authorized_clear =
            recovered.clear_all("authorized-tool", 5'010, "evidence-authorized-clear").accepted;
        trace_events += recovered.trace_snapshot().size();
    }

    {
        sdv::DiagnosticManager recovered(journal_path);
        register_dtcs(recovered, kDtcCount);
        active_after_clear_restart = static_cast<int>(recovered.active_dtcs().size());
    }
    {
        std::ofstream output(journal_path, std::ios::app);
        output << "intentional-corruption\n";
    }
    try {
        sdv::DiagnosticManager corrupted(journal_path);
    } catch (const std::runtime_error&) {
        corruption_rejected = true;
    }
    std::filesystem::remove(journal_path, remove_error);

    std::cout << "{\"registered_dtcs\":" << kDtcCount
              << ",\"failure_reports\":" << failure_reports
              << ",\"same_cycle_replays\":" << same_cycle_replays
              << ",\"confirmed_before_restart\":" << confirmed_before_restart
              << ",\"freeze_frames_before_restart\":" << freeze_frames_before_restart
              << ",\"recovered_confirmed\":" << recovered_confirmed
              << ",\"unauthorized_clears\":" << unauthorized_clears
              << ",\"security_lockouts\":" << security_lockouts
              << ",\"authorized_clear\":" << (authorized_clear ? "true" : "false")
              << ",\"active_after_clear_restart\":" << active_after_clear_restart
              << ",\"corruption_rejected\":" << (corruption_rejected ? "true" : "false")
              << ",\"trace_events\":" << trace_events << "}\n";

    const bool passed = failure_reports == 40 && same_cycle_replays == 20 &&
                        confirmed_before_restart == 20 &&
                        freeze_frames_before_restart == 20 && recovered_confirmed == 20 &&
                        unauthorized_clears == 30 && security_lockouts == 20 &&
                        authorized_clear && active_after_clear_restart == 0 &&
                        corruption_rejected;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

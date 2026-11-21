#pragma once

#include "sdv/types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sdv {

enum class DiagnosticSession { Default, Extended };

struct DiagnosticFreezeFrame {
    bool valid{false};
    std::int64_t captured_at_ms{0};
    VehicleSnapshot vehicle;
};

struct DiagnosticTroubleCode {
    std::uint32_t code{0};
    std::string name;
    std::uint8_t severity{0};
    bool test_failed{false};
    bool pending{false};
    bool confirmed{false};
    std::uint32_t occurrence_count{0};
    std::uint32_t consecutive_failure_cycles{0};
    std::uint32_t consecutive_pass_cycles{0};
    std::int64_t first_seen_ms{0};
    std::int64_t last_seen_ms{0};
    std::uint64_t last_failure_cycle{0};
    std::uint64_t last_pass_cycle{0};
    DiagnosticFreezeFrame freeze_frame;
    std::string last_trace_id;
};

struct DiagnosticOutcome {
    bool accepted{false};
    std::string code;
    std::string message;
    std::uint32_t seed{0};
};

struct DiagnosticTraceEvent {
    std::uint64_t sequence{0};
    std::int64_t time_ms{0};
    std::string trace_id;
    std::string client_id;
    std::string event;
    std::string detail;
};

class DiagnosticManager {
public:
    static constexpr std::int64_t kSessionTimeoutMs = 5'000;
    static constexpr std::int64_t kSecurityLockoutMs = 10'000;

    explicit DiagnosticManager(std::filesystem::path journal_path = {});

    bool register_dtc(std::uint32_t code, std::string name, std::uint8_t severity);
    DiagnosticOutcome begin_operation_cycle(std::uint64_t cycle_id, std::int64_t now_ms,
                                            std::string trace_id);
    DiagnosticOutcome report_test_result(std::uint32_t code, bool failed,
                                         const VehicleSnapshot& snapshot,
                                         std::int64_t now_ms, std::string trace_id);

    DiagnosticOutcome start_session(const std::string& client_id, DiagnosticSession session,
                                    std::int64_t now_ms, std::string trace_id);
    DiagnosticOutcome request_security_seed(const std::string& client_id,
                                            std::int64_t now_ms, std::string trace_id);
    DiagnosticOutcome submit_security_key(const std::string& client_id, std::uint32_t seed,
                                          std::uint32_t key, std::int64_t now_ms,
                                          std::string trace_id);
    DiagnosticOutcome clear_all(const std::string& client_id, std::int64_t now_ms,
                                std::string trace_id);

    std::vector<DiagnosticTroubleCode> active_dtcs() const;
    std::vector<DiagnosticTroubleCode> all_dtcs() const;
    std::vector<DiagnosticTraceEvent> trace_snapshot() const;

    static std::uint32_t calculate_test_key(std::uint32_t seed);

private:
    struct ClientContext {
        DiagnosticSession session{DiagnosticSession::Default};
        std::int64_t last_activity_ms{0};
        bool unlocked{false};
        bool seed_valid{false};
        std::uint32_t seed{0};
        std::uint8_t failed_keys{0};
        std::int64_t lockout_until_ms{0};
    };

    std::filesystem::path journal_path_;
    std::map<std::uint32_t, DiagnosticTroubleCode> dtcs_;
    std::map<std::string, ClientContext> clients_;
    std::vector<DiagnosticTraceEvent> trace_;
    std::uint64_t current_cycle_{0};
    std::uint64_t next_trace_sequence_{1};
    std::uint32_t next_seed_{0x6D2B79F5U};
    mutable std::mutex mutex_;

    void recover();
    bool append_upsert(const DiagnosticTroubleCode& dtc, std::string& error) const;
    bool append_clear(std::int64_t now_ms, const std::string& trace_id,
                      std::string& error) const;
    bool append_record(const std::string& content, std::string& error) const;
    void reset_dtc_statuses();
    ClientContext& refresh_client(const std::string& client_id, std::int64_t now_ms);
    void record(std::int64_t now_ms, std::string trace_id, std::string client_id,
                std::string event, std::string detail);
    static bool valid_identity(const std::string& value);
};

}  // namespace sdv

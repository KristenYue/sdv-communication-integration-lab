#include "sdv/diagnostic_manager.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sdv {
namespace {

std::uint32_t crc32(const std::string& value) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto character : value) {
        crc ^= static_cast<std::uint8_t>(character);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(static_cast<int>(crc & 1U)));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::string crc_hex(const std::string& value) {
    std::ostringstream output;
    output << std::hex << std::setw(8) << std::setfill('0') << crc32(value);
    return output.str();
}

std::string hex_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2U);
    for (const auto character : value) {
        const auto byte = static_cast<std::uint8_t>(character);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

bool hex_decode(const std::string& encoded, std::string& value) {
    if (encoded.size() % 2U != 0U) {
        return false;
    }
    value.clear();
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        const int high = hex_value(encoded.at(index));
        const int low = hex_value(encoded.at(index + 1U));
        if (high < 0 || low < 0) {
            return false;
        }
        value.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::istringstream input(value);
    std::string field;
    while (std::getline(input, field, delimiter)) {
        fields.push_back(field);
    }
    return fields;
}

std::string encode_dtc(const DiagnosticTroubleCode& dtc) {
    const auto& vehicle = dtc.freeze_frame.vehicle;
    std::ostringstream output;
    output << "U|" << dtc.code << '|' << hex_encode(dtc.name) << '|'
           << static_cast<unsigned int>(dtc.severity) << '|' << (dtc.test_failed ? 1 : 0)
           << '|' << (dtc.pending ? 1 : 0) << '|' << (dtc.confirmed ? 1 : 0) << '|'
           << dtc.occurrence_count << '|' << dtc.consecutive_failure_cycles << '|'
           << dtc.consecutive_pass_cycles << '|' << dtc.first_seen_ms << '|'
           << dtc.last_seen_ms << '|' << dtc.last_failure_cycle << '|'
           << dtc.last_pass_cycle << '|' << (dtc.freeze_frame.valid ? 1 : 0) << '|'
           << dtc.freeze_frame.captured_at_ms << '|'
           << static_cast<std::int64_t>(std::llround(vehicle.speed_kph * 100.0)) << '|'
           << vehicle.battery_percent << '|' << (vehicle.door_open ? 1 : 0);
    for (const auto pressure : vehicle.tire_kpa) {
        output << '|' << pressure;
    }
    output << '|' << static_cast<unsigned int>(vehicle.door_open_mask) << '|'
           << static_cast<unsigned int>(vehicle.door_lock_mask) << '|'
           << static_cast<unsigned int>(vehicle.wiper_state) << '|'
           << (vehicle.cloud_connected ? 1 : 0) << '|' << vehicle.signal_quality_percent
           << '|' << vehicle.upload_backlog << '|' << hex_encode(dtc.last_trace_id);
    return output.str();
}

bool parse_bool(const std::string& value, bool& output) {
    if (value != "0" && value != "1") {
        return false;
    }
    output = value == "1";
    return true;
}

bool decode_dtc(const std::vector<std::string>& fields, DiagnosticTroubleCode& dtc) {
    if (fields.size() != 30U || fields.front() != "U") {
        return false;
    }
    try {
        const auto code = std::stoul(fields.at(1U));
        const auto severity = std::stoul(fields.at(3U));
        if (code == 0U || code > 0xFFFFFFU || severity > 255U ||
            !hex_decode(fields.at(2U), dtc.name) ||
            !parse_bool(fields.at(4U), dtc.test_failed) ||
            !parse_bool(fields.at(5U), dtc.pending) ||
            !parse_bool(fields.at(6U), dtc.confirmed) ||
            !parse_bool(fields.at(14U), dtc.freeze_frame.valid) ||
            !parse_bool(fields.at(18U), dtc.freeze_frame.vehicle.door_open) ||
            !parse_bool(fields.at(26U), dtc.freeze_frame.vehicle.cloud_connected) ||
            !hex_decode(fields.at(29U), dtc.last_trace_id)) {
            return false;
        }
        dtc.code = static_cast<std::uint32_t>(code);
        dtc.severity = static_cast<std::uint8_t>(severity);
        dtc.occurrence_count = static_cast<std::uint32_t>(std::stoul(fields.at(7U)));
        dtc.consecutive_failure_cycles =
            static_cast<std::uint32_t>(std::stoul(fields.at(8U)));
        dtc.consecutive_pass_cycles =
            static_cast<std::uint32_t>(std::stoul(fields.at(9U)));
        dtc.first_seen_ms = std::stoll(fields.at(10U));
        dtc.last_seen_ms = std::stoll(fields.at(11U));
        dtc.last_failure_cycle = std::stoull(fields.at(12U));
        dtc.last_pass_cycle = std::stoull(fields.at(13U));
        dtc.freeze_frame.captured_at_ms = std::stoll(fields.at(15U));
        auto& vehicle = dtc.freeze_frame.vehicle;
        vehicle.speed_kph = static_cast<double>(std::stoll(fields.at(16U))) * 0.01;
        vehicle.battery_percent = std::stoi(fields.at(17U));
        for (std::size_t index = 0; index < vehicle.tire_kpa.size(); ++index) {
            vehicle.tire_kpa.at(index) = std::stoi(fields.at(19U + index));
        }
        vehicle.door_open_mask = static_cast<std::uint8_t>(std::stoul(fields.at(23U)));
        vehicle.door_lock_mask = static_cast<std::uint8_t>(std::stoul(fields.at(24U)));
        vehicle.wiper_state = static_cast<std::uint8_t>(std::stoul(fields.at(25U)));
        vehicle.signal_quality_percent = std::stoi(fields.at(27U));
        vehicle.upload_backlog = std::stoi(fields.at(28U));
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

DiagnosticManager::DiagnosticManager(std::filesystem::path journal_path)
    : journal_path_(std::move(journal_path)) {
    recover();
}

bool DiagnosticManager::valid_identity(const std::string& value) {
    return !value.empty() && value.size() <= 64U;
}

void DiagnosticManager::record(std::int64_t now_ms, std::string trace_id,
                               std::string client_id, std::string event,
                               std::string detail) {
    trace_.push_back(DiagnosticTraceEvent{next_trace_sequence_++, now_ms, std::move(trace_id),
                                          std::move(client_id), std::move(event),
                                          std::move(detail)});
}

bool DiagnosticManager::register_dtc(std::uint32_t code, std::string name,
                                     std::uint8_t severity) {
    std::lock_guard lock(mutex_);
    if (code == 0U || code > 0xFFFFFFU || !valid_identity(name)) {
        return false;
    }
    if (const auto existing = dtcs_.find(code); existing != dtcs_.end()) {
        return existing->second.name == name && existing->second.severity == severity;
    }
    dtcs_.emplace(code, DiagnosticTroubleCode{code, std::move(name), severity});
    return true;
}

DiagnosticOutcome DiagnosticManager::begin_operation_cycle(std::uint64_t cycle_id,
                                                           std::int64_t now_ms,
                                                           std::string trace_id) {
    std::lock_guard lock(mutex_);
    if (cycle_id == 0U || cycle_id <= current_cycle_ || now_ms < 0 ||
        !valid_identity(trace_id)) {
        return {false, "INVALID_CYCLE", "operation cycle must increase monotonically", 0U};
    }
    current_cycle_ = cycle_id;
    record(now_ms, std::move(trace_id), "diagnostic-monitor", "CYCLE_STARTED",
           "operation cycle " + std::to_string(cycle_id));
    return {true, "OK", "operation cycle started", 0U};
}

DiagnosticOutcome DiagnosticManager::report_test_result(std::uint32_t code, bool failed,
                                                        const VehicleSnapshot& snapshot,
                                                        std::int64_t now_ms,
                                                        std::string trace_id) {
    std::lock_guard lock(mutex_);
    if (current_cycle_ == 0U) {
        return {false, "NO_OPERATION_CYCLE", "start an operation cycle first", 0U};
    }
    if (now_ms < 0 || !valid_identity(trace_id)) {
        return {false, "INVALID_REPORT", "fault report metadata is invalid", 0U};
    }
    const auto found = dtcs_.find(code);
    if (found == dtcs_.end()) {
        return {false, "DTC_NOT_REGISTERED", "fault monitor is not registered", 0U};
    }
    auto updated = found->second;
    if (failed) {
        if (updated.last_failure_cycle != current_cycle_) {
            ++updated.occurrence_count;
            ++updated.consecutive_failure_cycles;
        }
        updated.test_failed = true;
        updated.pending = true;
        updated.confirmed = updated.confirmed || updated.consecutive_failure_cycles >= 2U;
        updated.consecutive_pass_cycles = 0U;
        updated.last_failure_cycle = current_cycle_;
        updated.last_seen_ms = now_ms;
        if (updated.occurrence_count == 1U) {
            updated.first_seen_ms = now_ms;
        }
        if (!updated.freeze_frame.valid) {
            updated.freeze_frame = DiagnosticFreezeFrame{true, now_ms, snapshot};
        }
    } else if (updated.last_failure_cycle != current_cycle_ &&
               updated.last_pass_cycle != current_cycle_) {
        updated.test_failed = false;
        updated.consecutive_failure_cycles = 0U;
        ++updated.consecutive_pass_cycles;
        updated.last_pass_cycle = current_cycle_;
        updated.last_seen_ms = now_ms;
        if (updated.consecutive_pass_cycles >= 2U) {
            updated.pending = false;
        }
        if (updated.consecutive_pass_cycles >= 3U) {
            updated.confirmed = false;
        }
    }
    updated.last_trace_id = trace_id;
    std::string error;
    if (!append_upsert(updated, error)) {
        return {false, "JOURNAL_ERROR", error, 0U};
    }
    found->second = updated;
    record(now_ms, std::move(trace_id), "diagnostic-monitor",
           failed ? "DTC_TEST_FAILED" : "DTC_TEST_PASSED",
           "DTC " + std::to_string(code) + " state updated");
    return {true, "OK", "DTC state updated", 0U};
}

DiagnosticManager::ClientContext& DiagnosticManager::refresh_client(
    const std::string& client_id, std::int64_t now_ms) {
    auto& context = clients_[client_id];
    if (context.last_activity_ms > 0 &&
        (now_ms < context.last_activity_ms ||
         now_ms - context.last_activity_ms > kSessionTimeoutMs)) {
        const auto lockout_until_ms = context.lockout_until_ms;
        context = ClientContext{};
        context.lockout_until_ms = lockout_until_ms;
    }
    context.last_activity_ms = now_ms;
    return context;
}

DiagnosticOutcome DiagnosticManager::start_session(const std::string& client_id,
                                                   DiagnosticSession session,
                                                   std::int64_t now_ms,
                                                   std::string trace_id) {
    std::lock_guard lock(mutex_);
    if (!valid_identity(client_id) || !valid_identity(trace_id) || now_ms < 0) {
        return {false, "INVALID_SESSION_REQUEST", "session metadata is invalid", 0U};
    }
    auto& context = refresh_client(client_id, now_ms);
    context.session = session;
    context.unlocked = false;
    context.seed_valid = false;
    context.failed_keys = 0U;
    record(now_ms, std::move(trace_id), client_id, "SESSION_CHANGED",
           session == DiagnosticSession::Extended ? "extended" : "default");
    return {true, "OK", "diagnostic session changed", 0U};
}

DiagnosticOutcome DiagnosticManager::request_security_seed(const std::string& client_id,
                                                           std::int64_t now_ms,
                                                           std::string trace_id) {
    std::lock_guard lock(mutex_);
    if (!valid_identity(client_id) || !valid_identity(trace_id) || now_ms < 0) {
        return {false, "INVALID_SECURITY_REQUEST", "security metadata is invalid", 0U};
    }
    auto& context = refresh_client(client_id, now_ms);
    if (context.session != DiagnosticSession::Extended) {
        return {false, "SESSION_DENIED", "extended session is required", 0U};
    }
    if (now_ms < context.lockout_until_ms) {
        return {false, "SECURITY_LOCKED", "security access is temporarily locked", 0U};
    }
    next_seed_ ^= next_seed_ << 13U;
    next_seed_ ^= next_seed_ >> 17U;
    next_seed_ ^= next_seed_ << 5U;
    if (next_seed_ == 0U) {
        next_seed_ = 0x6D2B79F5U;
    }
    context.seed = next_seed_;
    context.seed_valid = true;
    record(now_ms, std::move(trace_id), client_id, "SECURITY_SEED_ISSUED",
           "non-cryptographic project challenge issued");
    return {true, "OK", "security seed issued", context.seed};
}

std::uint32_t DiagnosticManager::calculate_test_key(std::uint32_t seed) {
    const auto rotated = static_cast<std::uint32_t>((seed << 7U) | (seed >> 25U));
    return rotated ^ 0xA5C39E71U;
}

DiagnosticOutcome DiagnosticManager::submit_security_key(const std::string& client_id,
                                                         std::uint32_t seed,
                                                         std::uint32_t key,
                                                         std::int64_t now_ms,
                                                         std::string trace_id) {
    std::lock_guard lock(mutex_);
    if (!valid_identity(client_id) || !valid_identity(trace_id) || now_ms < 0) {
        return {false, "INVALID_SECURITY_REQUEST", "security metadata is invalid", 0U};
    }
    auto& context = refresh_client(client_id, now_ms);
    if (context.session != DiagnosticSession::Extended) {
        return {false, "SESSION_DENIED", "extended session is required", 0U};
    }
    if (now_ms < context.lockout_until_ms) {
        return {false, "SECURITY_LOCKED", "security access is temporarily locked", 0U};
    }
    if (!context.seed_valid || seed != context.seed || key != calculate_test_key(seed)) {
        ++context.failed_keys;
        if (context.failed_keys >= 3U) {
            context.lockout_until_ms = now_ms + kSecurityLockoutMs;
            context.failed_keys = 0U;
            context.seed_valid = false;
            record(now_ms, std::move(trace_id), client_id, "SECURITY_LOCKED",
                   "three invalid keys triggered lockout");
            return {false, "SECURITY_LOCKED", "three invalid keys triggered lockout", 0U};
        }
        record(now_ms, std::move(trace_id), client_id, "INVALID_KEY",
               "security key rejected");
        return {false, "INVALID_KEY", "security key rejected", 0U};
    }
    context.unlocked = true;
    context.seed_valid = false;
    context.failed_keys = 0U;
    record(now_ms, std::move(trace_id), client_id, "SECURITY_UNLOCKED",
           "diagnostic clear access granted for current session");
    return {true, "OK", "security access granted", 0U};
}

void DiagnosticManager::reset_dtc_statuses() {
    for (auto& [code, dtc] : dtcs_) {
        static_cast<void>(code);
        dtc.test_failed = false;
        dtc.pending = false;
        dtc.confirmed = false;
        dtc.consecutive_failure_cycles = 0U;
        dtc.consecutive_pass_cycles = 0U;
        dtc.freeze_frame = DiagnosticFreezeFrame{};
    }
}

DiagnosticOutcome DiagnosticManager::clear_all(const std::string& client_id,
                                               std::int64_t now_ms,
                                               std::string trace_id) {
    std::lock_guard lock(mutex_);
    if (!valid_identity(client_id) || !valid_identity(trace_id) || now_ms < 0) {
        return {false, "INVALID_CLEAR_REQUEST", "clear metadata is invalid", 0U};
    }
    auto& context = refresh_client(client_id, now_ms);
    if (context.session != DiagnosticSession::Extended) {
        return {false, "SESSION_DENIED", "extended session is required", 0U};
    }
    if (!context.unlocked) {
        return {false, "SECURITY_DENIED", "security access is required", 0U};
    }
    std::string error;
    if (!append_clear(now_ms, trace_id, error)) {
        return {false, "JOURNAL_ERROR", error, 0U};
    }
    reset_dtc_statuses();
    context.unlocked = false;
    record(now_ms, std::move(trace_id), client_id, "DTC_CLEARED",
           "all DTC status and freeze frames cleared");
    return {true, "OK", "all DTCs cleared", 0U};
}

std::vector<DiagnosticTroubleCode> DiagnosticManager::active_dtcs() const {
    std::lock_guard lock(mutex_);
    std::vector<DiagnosticTroubleCode> result;
    for (const auto& [code, dtc] : dtcs_) {
        static_cast<void>(code);
        if (dtc.test_failed || dtc.pending || dtc.confirmed) {
            result.push_back(dtc);
        }
    }
    return result;
}

std::vector<DiagnosticTroubleCode> DiagnosticManager::all_dtcs() const {
    std::lock_guard lock(mutex_);
    std::vector<DiagnosticTroubleCode> result;
    result.reserve(dtcs_.size());
    for (const auto& [code, dtc] : dtcs_) {
        static_cast<void>(code);
        result.push_back(dtc);
    }
    return result;
}

std::vector<DiagnosticTraceEvent> DiagnosticManager::trace_snapshot() const {
    std::lock_guard lock(mutex_);
    return trace_;
}

bool DiagnosticManager::append_record(const std::string& content, std::string& error) const {
    if (journal_path_.empty()) {
        error.clear();
        return true;
    }
    std::error_code directory_error;
    if (!journal_path_.parent_path().empty()) {
        std::filesystem::create_directories(journal_path_.parent_path(), directory_error);
        if (directory_error) {
            error = "cannot create diagnostic journal directory: " + directory_error.message();
            return false;
        }
    }
    std::ofstream output(journal_path_, std::ios::binary | std::ios::app);
    if (!output) {
        error = "cannot open diagnostic journal";
        return false;
    }
    output << content << '|' << crc_hex(content) << '\n';
    output.flush();
    if (!output) {
        error = "cannot flush diagnostic journal";
        return false;
    }
    error.clear();
    return true;
}

bool DiagnosticManager::append_upsert(const DiagnosticTroubleCode& dtc,
                                      std::string& error) const {
    return append_record(encode_dtc(dtc), error);
}

bool DiagnosticManager::append_clear(std::int64_t now_ms, const std::string& trace_id,
                                     std::string& error) const {
    return append_record("C|" + std::to_string(now_ms) + '|' + hex_encode(trace_id), error);
}

void DiagnosticManager::recover() {
    if (journal_path_.empty() || !std::filesystem::exists(journal_path_)) {
        return;
    }
    std::ifstream input(journal_path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read diagnostic journal");
    }
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        const auto separator = line.rfind('|');
        if (separator == std::string::npos) {
            throw std::runtime_error("diagnostic journal line has no checksum");
        }
        const auto content = line.substr(0U, separator);
        if (line.substr(separator + 1U) != crc_hex(content)) {
            throw std::runtime_error("diagnostic journal checksum mismatch at line " +
                                     std::to_string(line_number));
        }
        const auto fields = split(content, '|');
        if (!fields.empty() && fields.front() == "C" && fields.size() == 3U) {
            std::string trace_id;
            if (!hex_decode(fields.at(2U), trace_id)) {
                throw std::runtime_error("diagnostic clear record has invalid trace");
            }
            reset_dtc_statuses();
            continue;
        }
        DiagnosticTroubleCode dtc;
        if (!decode_dtc(fields, dtc)) {
            throw std::runtime_error("diagnostic journal schema invalid at line " +
                                     std::to_string(line_number));
        }
        current_cycle_ = std::max(current_cycle_,
                                  std::max(dtc.last_failure_cycle, dtc.last_pass_cycle));
        dtcs_[dtc.code] = std::move(dtc);
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while reading diagnostic journal");
    }
}

}  // namespace sdv

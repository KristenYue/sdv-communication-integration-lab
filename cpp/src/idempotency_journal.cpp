#include "sdv/idempotency_journal.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace sdv {
namespace {

std::string hex_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const auto character : value) {
        const auto byte = static_cast<std::uint8_t>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0FU]);
    }
    return encoded;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

bool hex_decode(const std::string& encoded, std::string& value) {
    if (encoded.size() % 2U != 0U) {
        return false;
    }
    value.clear();
    value.reserve(encoded.size() / 2U);
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

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::istringstream input(value);
    std::string field;
    while (std::getline(input, field, delimiter)) {
        fields.push_back(field);
    }
    return fields;
}

bool encode_payload(const ServiceResponsePayload& payload, std::string& tag, std::string& data) {
    if (std::holds_alternative<std::monostate>(payload)) {
        tag = "N";
        data.clear();
        return true;
    }
    if (const auto* result = std::get_if<DoorLockResult>(&payload)) {
        tag = "D";
        data = std::to_string(result->requested_mask) + "," + (result->locked ? "1" : "0");
        return true;
    }
    return false;
}

bool decode_payload(const std::string& tag, const std::string& data,
                    ServiceResponsePayload& payload) {
    if (tag == "N" && data.empty()) {
        payload = std::monostate{};
        return true;
    }
    if (tag == "D") {
        const auto comma = data.find(',');
        if (comma == std::string::npos) {
            return false;
        }
        try {
            const auto mask = std::stoul(data.substr(0, comma));
            const auto locked = data.substr(comma + 1U);
            if (mask > 255U || (locked != "0" && locked != "1")) {
                return false;
            }
            payload = DoorLockResult{static_cast<std::uint8_t>(mask), locked == "1"};
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

struct RecoveryState {
    std::string fingerprint;
    ServiceResponse response{false, "COMPLETION_UNKNOWN",
                             "request began before process exit; side effects may have occurred",
                             std::monostate{}};
};

}  // namespace

IdempotencyJournal::IdempotencyJournal(std::filesystem::path path) : path_(std::move(path)) {}

bool IdempotencyJournal::append_record(const std::string& content, std::string& error) {
    std::error_code directory_error;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), directory_error);
        if (directory_error) {
            error = "cannot create journal directory: " + directory_error.message();
            return false;
        }
    }
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) {
        error = "cannot open idempotency journal";
        return false;
    }
    output << content << '|' << crc_hex(content) << '\n';
    output.flush();
    if (!output) {
        error = "cannot flush idempotency journal";
        return false;
    }
    error.clear();
    return true;
}

bool IdempotencyJournal::append_begin(const std::string& request_id,
                                      const std::string& fingerprint, std::string& error) {
    return append_record("B|" + hex_encode(request_id) + '|' + hex_encode(fingerprint), error);
}

bool IdempotencyJournal::append_complete(const std::string& request_id,
                                         const std::string& fingerprint,
                                         const ServiceResponse& response, std::string& error) {
    std::string tag;
    std::string data;
    if (!encode_payload(response.payload, tag, data)) {
        error = "response payload type is not journal-serializable";
        return false;
    }
    const std::string content = "C|" + hex_encode(request_id) + '|' + hex_encode(fingerprint) +
                                '|' + (response.accepted ? "1" : "0") + '|' +
                                hex_encode(response.code) + '|' + hex_encode(response.message) +
                                '|' + tag + '|' + hex_encode(data);
    return append_record(content, error);
}

bool IdempotencyJournal::recover(std::vector<RecoveredJournalRequest>& requests,
                                 std::string& error) const {
    requests.clear();
    if (!std::filesystem::exists(path_)) {
        error.clear();
        return true;
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        error = "cannot read idempotency journal";
        return false;
    }
    std::map<std::string, RecoveryState> recovered;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto checksum_separator = line.rfind('|');
        if (checksum_separator == std::string::npos) {
            error = "journal line " + std::to_string(line_number) + " has no checksum";
            return false;
        }
        const auto content = line.substr(0, checksum_separator);
        if (line.substr(checksum_separator + 1U) != crc_hex(content)) {
            error = "journal line " + std::to_string(line_number) + " checksum mismatch";
            return false;
        }
        const auto fields = split(content, '|');
        if (fields.empty() ||
            (fields.size() != 3U && fields.front() == "B") ||
            (fields.size() != 8U && fields.front() == "C") ||
            (fields.front() != "B" && fields.front() != "C")) {
            error = "journal line " + std::to_string(line_number) + " has invalid schema";
            return false;
        }
        std::string request_id;
        std::string fingerprint;
        if (!hex_decode(fields.at(1U), request_id) ||
            !hex_decode(fields.at(2U), fingerprint)) {
            error = "journal line " + std::to_string(line_number) + " has invalid hex fields";
            return false;
        }
        if (fields.front() == "B") {
            const auto [iterator, inserted] = recovered.emplace(request_id, RecoveryState{fingerprint});
            if (!inserted && iterator->second.fingerprint != fingerprint) {
                error = "journal contains conflicting begin records";
                return false;
            }
            continue;
        }
        const auto entry = recovered.find(request_id);
        if (entry == recovered.end() || entry->second.fingerprint != fingerprint) {
            error = "journal completion has no matching begin record";
            return false;
        }
        std::string code;
        std::string message;
        std::string payload_data;
        ServiceResponsePayload payload;
        if ((fields.at(3U) != "0" && fields.at(3U) != "1") ||
            !hex_decode(fields.at(4U), code) || !hex_decode(fields.at(5U), message) ||
            !hex_decode(fields.at(7U), payload_data) ||
            !decode_payload(fields.at(6U), payload_data, payload)) {
            error = "journal completion has invalid response fields";
            return false;
        }
        entry->second.response =
            ServiceResponse{fields.at(3U) == "1", std::move(code), std::move(message),
                            std::move(payload)};
    }
    if (!input.eof()) {
        error = "failed while reading idempotency journal";
        return false;
    }
    requests.reserve(recovered.size());
    for (auto& [request_id, state] : recovered) {
        requests.push_back(RecoveredJournalRequest{request_id, std::move(state.fingerprint),
                                                   std::move(state.response)});
    }
    error.clear();
    return true;
}

}  // namespace sdv

#include "sdv/vehicle_gateway.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = line.find('\t', start);
        if (end == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, end - start));
        start = end + 1U;
    }
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "?";
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::uint8_t hex_byte(const std::string& text, std::size_t offset) {
    const auto digit = [](char character) -> unsigned int {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned int>(character - '0');
        }
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (character >= 'a' && character <= 'f') {
            return static_cast<unsigned int>(character - 'a' + 10);
        }
        throw std::invalid_argument("payload contains a non-hexadecimal character");
    };
    return static_cast<std::uint8_t>(
        (digit(text.at(offset)) << 4U) | digit(text.at(offset + 1U)));
}

std::array<std::uint8_t, 8> parse_payload(const std::string& text) {
    if (text.size() != 16U) {
        throw std::invalid_argument("payload must encode exactly eight bytes");
    }
    std::array<std::uint8_t, 8> payload{};
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload.at(index) = hex_byte(text, index * 2U);
    }
    return payload;
}

void emit_result(const sdv::IngestResult& result, std::uint32_t frame_id,
                 const std::string& trace_id) {
    std::cout << "{\"kind\":\"frame_result\",\"accepted\":"
              << (result.accepted ? "true" : "false") << ",\"code\":\""
              << json_escape(result.code) << "\",\"message\":\""
              << json_escape(result.message) << "\",\"frame_id\":" << frame_id
              << ",\"trace_id\":\"" << json_escape(trace_id) << "\"}" << std::endl;
}

void emit_cli_error(const std::string& message) {
    std::cout << "{\"kind\":\"cli_error\",\"accepted\":false,"
                 "\"code\":\"CLI_INVALID\",\"message\":\""
              << json_escape(message) << "\"}" << std::endl;
}

}  // namespace

int main() {
    sdv::VehicleGateway gateway;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto parts = split_tabs(line);
        if (parts.empty()) continue;
        try {
            if (parts.at(0) == "RESET") {
                if (parts.size() != 1U) {
                    throw std::invalid_argument("RESET does not accept arguments");
                }
                gateway = sdv::VehicleGateway{};
                std::cout << "{\"kind\":\"reset\",\"ok\":true}" << std::endl;
            } else if (parts.at(0) == "FRAME") {
                if (parts.size() != 6U) {
                    throw std::invalid_argument(
                        "FRAME requires id, dlc, payload, timestamp and trace_id");
                }
                const auto identifier =
                    static_cast<std::uint32_t>(std::stoul(parts.at(1), nullptr, 0));
                const auto dlc =
                    static_cast<std::uint8_t>(std::stoul(parts.at(2), nullptr, 10));
                const auto timestamp =
                    static_cast<std::int64_t>(std::stoll(parts.at(4), nullptr, 10));
                const sdv::CanFrame frame{
                    identifier, dlc, parse_payload(parts.at(3)), timestamp, parts.at(5)};
                emit_result(gateway.ingest(frame), frame.id, frame.trace_id);
            } else if (parts.at(0) == "SNAPSHOT") {
                if (parts.size() != 3U) {
                    throw std::invalid_argument("SNAPSHOT requires now_ms and max_age_ms");
                }
                const auto snapshot =
                    gateway.snapshot(std::stoll(parts.at(1)), std::stoll(parts.at(2)));
                std::cout << "{\"kind\":\"snapshot\",\"speed_kph\":"
                          << snapshot.speed_kph << ",\"battery_percent\":"
                          << snapshot.battery_percent << ",\"door_open\":"
                          << (snapshot.door_open ? "true" : "false")
                          << ",\"vehicle_status_stale\":"
                          << (snapshot.vehicle_status.stale ? "true" : "false")
                          << ",\"tire_pressure_stale\":"
                          << (snapshot.tire_pressure.stale ? "true" : "false")
                          << ",\"body_status_stale\":"
                          << (snapshot.body_status.stale ? "true" : "false")
                          << ",\"tbox_health_stale\":"
                          << (snapshot.tbox_health.stale ? "true" : "false")
                          << ",\"door_open_mask\":"
                          << static_cast<unsigned int>(snapshot.door_open_mask)
                          << ",\"door_lock_mask\":"
                          << static_cast<unsigned int>(snapshot.door_lock_mask)
                          << ",\"wiper_state\":"
                          << static_cast<unsigned int>(snapshot.wiper_state)
                          << ",\"cloud_connected\":"
                          << (snapshot.cloud_connected ? "true" : "false")
                          << ",\"signal_quality_percent\":"
                          << snapshot.signal_quality_percent
                          << ",\"upload_backlog\":" << snapshot.upload_backlog
                          << ",\"tire_kpa\":[" << snapshot.tire_kpa.at(0) << ','
                          << snapshot.tire_kpa.at(1) << ',' << snapshot.tire_kpa.at(2) << ','
                          << snapshot.tire_kpa.at(3) << ']'
                          << "}" << std::endl;
            } else if (parts.at(0) == "QUIT") {
                std::cout << "{\"kind\":\"quit\",\"ok\":true}" << std::endl;
                return EXIT_SUCCESS;
            } else {
                throw std::invalid_argument("unknown command");
            }
        } catch (const std::exception& error) {
            emit_cli_error(error.what());
        }
    }
    return EXIT_SUCCESS;
}

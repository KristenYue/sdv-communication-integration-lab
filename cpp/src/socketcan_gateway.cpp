#include "sdv/vehicle_gateway.hpp"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string interface{"vcan0"};
    std::size_t count{2};
    std::string output;
};

struct ObservedFrame {
    std::uint32_t id{0};
    std::uint8_t dlc{0};
    std::string payload_hex;
    std::string code;
    bool accepted{false};
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--interface" && index + 1 < argc) {
            options.interface = argv[++index];
        } else if (argument == "--count" && index + 1 < argc) {
            options.count = std::stoul(argv[++index]);
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else {
            throw std::invalid_argument("usage: --interface NAME --count N --output PATH");
        }
    }
    if (options.interface.empty() || options.interface.size() >= IFNAMSIZ) {
        throw std::invalid_argument("invalid SocketCAN interface name");
    }
    if (options.count == 0U || options.count > 10'000U) {
        throw std::invalid_argument("count must be between 1 and 10000");
    }
    if (options.output.empty()) {
        throw std::invalid_argument("output path is required");
    }
    return options;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string payload_hex(const can_frame& frame) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < 8U; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(frame.data[index]);
    }
    return output.str();
}

void write_report(const Options& options, const std::vector<ObservedFrame>& frames) {
    std::ofstream output(options.output);
    if (!output) throw std::runtime_error("cannot open output report");
    output << "{\n  \"schema_version\": \"sdv-cpp-socketcan-v1\",\n"
           << "  \"socketcan_used\": true,\n"
           << "  \"transport_boundary\": \"linux_virtual_can_not_physical_vehicle_bus\",\n"
           << "  \"interface\": \"" << options.interface << "\",\n"
           << "  \"frame_count\": " << frames.size() << ",\n"
           << "  \"results\": [\n";
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        output << "    {\"frame_id\": " << frame.id << ", \"dlc\": "
               << static_cast<unsigned int>(frame.dlc) << ", \"payload_hex\": \""
               << frame.payload_hex << "\", \"accepted\": "
               << (frame.accepted ? "true" : "false") << ", \"code\": \""
               << frame.code << "\"}";
        if (index + 1U != frames.size()) output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const int socket_fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (socket_fd < 0) throw std::runtime_error("failed to open SocketCAN socket");

        ifreq request{};
        std::strncpy(request.ifr_name, options.interface.c_str(), IFNAMSIZ - 1U);
        if (::ioctl(socket_fd, SIOCGIFINDEX, &request) < 0) {
            ::close(socket_fd);
            throw std::runtime_error("failed to resolve SocketCAN interface index");
        }
        sockaddr_can address{};
        address.can_family = AF_CAN;
        address.can_ifindex = request.ifr_ifindex;
        if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(socket_fd);
            throw std::runtime_error("failed to bind SocketCAN interface");
        }

        sdv::VehicleGateway gateway;
        std::vector<ObservedFrame> observed;
        observed.reserve(options.count);
        while (observed.size() < options.count) {
            can_frame native_frame{};
            const auto bytes = ::read(socket_fd, &native_frame, sizeof(native_frame));
            if (bytes != static_cast<ssize_t>(sizeof(native_frame))) {
                ::close(socket_fd);
                throw std::runtime_error("incomplete SocketCAN frame read");
            }
            if ((native_frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0U) {
                continue;
            }
            sdv::CanFrame frame{};
            frame.id = native_frame.can_id & CAN_SFF_MASK;
            frame.dlc = native_frame.can_dlc;
            std::copy(std::begin(native_frame.data), std::end(native_frame.data), frame.data.begin());
            frame.bus_time_ms = now_ms();
            frame.trace_id = "socketcan-" + std::to_string(observed.size() + 1U);
            const auto result = gateway.ingest(frame);
            observed.push_back(ObservedFrame{frame.id, frame.dlc, payload_hex(native_frame),
                                             result.code, result.accepted});
        }
        ::close(socket_fd);
        write_report(options, observed);
        std::cout << "processed " << observed.size() << " SocketCAN frames\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

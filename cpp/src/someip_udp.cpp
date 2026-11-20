#include "sdv/someip_udp.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sdv {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
using SocketLength = int;

void close_socket(NativeSocket socket) {
    if (socket != kInvalidSocket) {
        closesocket(socket);
    }
}

std::string socket_error() { return "Winsock error " + std::to_string(WSAGetLastError()); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
using SocketLength = socklen_t;

void close_socket(NativeSocket socket) {
    if (socket != kInvalidSocket) {
        close(socket);
    }
}

std::string socket_error() { return std::strerror(errno); }
#endif

bool set_receive_timeout(NativeSocket socket, int timeout_ms, std::string& error) {
#ifdef _WIN32
    const auto value = static_cast<DWORD>(timeout_ms);
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) == SOCKET_ERROR) {
#else
    const timeval value{timeout_ms / 1'000, (timeout_ms % 1'000) * 1'000};
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) < 0) {
#endif
        error = socket_error();
        return false;
    }
    return true;
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

SomeIpTransportResult transport_error(std::string code, std::string message) {
    return SomeIpTransportResult{false, std::move(code), std::move(message), {}};
}

}  // namespace

struct SomeIpUdpServer::Impl {
    NativeSocket socket{kInvalidSocket};
    std::string error;
#ifdef _WIN32
    bool winsock_started{false};
#endif

    Impl(std::uint16_t port, int timeout_ms) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = socket_error();
            return;
        }
        winsock_started = true;
#endif
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == kInvalidSocket) {
            error = socket_error();
            return;
        }
        if (!set_receive_timeout(socket, timeout_ms, error)) {
            return;
        }
        const auto address = loopback_address(port);
#ifdef _WIN32
        const auto result = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (result == SOCKET_ERROR) {
#else
        const auto result = bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (result < 0) {
#endif
            error = socket_error();
        }
    }

    ~Impl() {
        close_socket(socket);
#ifdef _WIN32
        if (winsock_started) {
            WSACleanup();
        }
#endif
    }
};

SomeIpUdpServer::SomeIpUdpServer(std::uint16_t port, int timeout_ms)
    : impl_(std::make_unique<Impl>(port, timeout_ms)) {}

SomeIpUdpServer::~SomeIpUdpServer() = default;
SomeIpUdpServer::SomeIpUdpServer(SomeIpUdpServer&&) noexcept = default;
SomeIpUdpServer& SomeIpUdpServer::operator=(SomeIpUdpServer&&) noexcept = default;

bool SomeIpUdpServer::valid() const noexcept {
    return impl_ != nullptr && impl_->socket != kInvalidSocket && impl_->error.empty();
}

const std::string& SomeIpUdpServer::error() const noexcept {
    static const std::string empty;
    return impl_ != nullptr ? impl_->error : empty;
}

SomeIpTransportResult SomeIpUdpServer::serve_once(
    const std::function<SomeIpMessage(const SomeIpMessage&)>& handler) {
    if (!valid() || !handler) {
        return transport_error("SERVER_NOT_READY", error());
    }
    std::array<std::uint8_t, 8'192> buffer{};
    sockaddr_in peer{};
    SocketLength peer_size = sizeof(peer);
#ifdef _WIN32
    const auto received = recvfrom(impl_->socket, reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()), 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (received == SOCKET_ERROR) {
#else
    const auto received = recvfrom(impl_->socket, buffer.data(), buffer.size(), 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (received < 0) {
#endif
        return transport_error("RECEIVE_FAILED", socket_error());
    }
    const std::vector<std::uint8_t> bytes(buffer.begin(), buffer.begin() + received);
    const auto decoded = decode_someip(bytes);
    if (!decoded.accepted) {
        return transport_error(decoded.code, decoded.message);
    }
    const auto response = encode_someip(handler(decoded.value));
#ifdef _WIN32
    const auto sent = sendto(impl_->socket, reinterpret_cast<const char*>(response.data()),
                             static_cast<int>(response.size()), 0,
                             reinterpret_cast<const sockaddr*>(&peer), peer_size);
    if (sent == SOCKET_ERROR || static_cast<std::size_t>(sent) != response.size()) {
#else
    const auto sent = sendto(impl_->socket, response.data(), response.size(), 0,
                             reinterpret_cast<const sockaddr*>(&peer), peer_size);
    if (sent < 0 || static_cast<std::size_t>(sent) != response.size()) {
#endif
        return transport_error("SEND_FAILED", socket_error());
    }
    return SomeIpTransportResult{true, "OK", "request served", decoded.value};
}

SomeIpTransportResult request_someip_udp(std::uint16_t port, const SomeIpMessage& request,
                                         int timeout_ms) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return transport_error("SOCKET_INIT_FAILED", socket_error());
    }
#endif
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
#ifdef _WIN32
        WSACleanup();
#endif
        return transport_error("SOCKET_CREATE_FAILED", socket_error());
    }
    std::string error;
    if (!set_receive_timeout(socket, timeout_ms, error)) {
        close_socket(socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return transport_error("SOCKET_CONFIG_FAILED", error);
    }
    const auto address = loopback_address(port);
    const auto bytes = encode_someip(request);
#ifdef _WIN32
    const auto sent = sendto(socket, reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size()), 0,
                             reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const bool send_failed = sent == SOCKET_ERROR || static_cast<std::size_t>(sent) != bytes.size();
#else
    const auto sent = sendto(socket, bytes.data(), bytes.size(), 0,
                             reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const bool send_failed = sent < 0 || static_cast<std::size_t>(sent) != bytes.size();
#endif
    if (send_failed) {
        error = socket_error();
        close_socket(socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return transport_error("SEND_FAILED", error);
    }
    std::array<std::uint8_t, 8'192> buffer{};
#ifdef _WIN32
    const auto received = recvfrom(socket, reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()), 0, nullptr, nullptr);
    const bool receive_failed = received == SOCKET_ERROR;
#else
    const auto received = recvfrom(socket, buffer.data(), buffer.size(), 0, nullptr, nullptr);
    const bool receive_failed = received < 0;
#endif
    if (receive_failed) {
        error = socket_error();
        close_socket(socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return transport_error("RECEIVE_FAILED", error);
    }
    close_socket(socket);
#ifdef _WIN32
    WSACleanup();
#endif
    const std::vector<std::uint8_t> response_bytes(buffer.begin(), buffer.begin() + received);
    const auto decoded = decode_someip(response_bytes);
    if (!decoded.accepted) {
        return transport_error(decoded.code, decoded.message);
    }
    if (decoded.value.client_id != request.client_id ||
        decoded.value.session_id != request.session_id) {
        return transport_error("REQUEST_ID_MISMATCH", "response does not match request identity");
    }
    return SomeIpTransportResult{true, "OK", "response received", decoded.value};
}

}  // namespace sdv

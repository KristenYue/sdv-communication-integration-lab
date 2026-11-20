#pragma once

#include "sdv/someip.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sdv {

struct SomeIpTransportResult {
    bool accepted{false};
    std::string code;
    std::string message;
    SomeIpMessage value;
};

class SomeIpUdpServer {
public:
    SomeIpUdpServer(std::uint16_t port, int timeout_ms);
    ~SomeIpUdpServer();
    SomeIpUdpServer(const SomeIpUdpServer&) = delete;
    SomeIpUdpServer& operator=(const SomeIpUdpServer&) = delete;
    SomeIpUdpServer(SomeIpUdpServer&&) noexcept;
    SomeIpUdpServer& operator=(SomeIpUdpServer&&) noexcept;

    bool valid() const noexcept;
    const std::string& error() const noexcept;
    SomeIpTransportResult serve_once(
        const std::function<SomeIpMessage(const SomeIpMessage&)>& handler,
        bool send_response = true);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

SomeIpTransportResult request_someip_udp(std::uint16_t port, const SomeIpMessage& request,
                                         int timeout_ms);

}  // namespace sdv

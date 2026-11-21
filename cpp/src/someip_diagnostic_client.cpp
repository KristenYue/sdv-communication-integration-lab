#include "sdv/diagnostic_manager.hpp"
#include "sdv/someip_udp.hpp"
#include "sdv/vehicle_someip_adapter.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool exchange(std::uint16_t port, const sdv::SomeIpMessage& request,
              std::uint16_t method, sdv::DiagnosticWireResponse& output,
              std::string& error) {
    const auto result = sdv::request_someip_udp(port, request, 1'000);
    if (!result.accepted) {
        error = "method=" + std::to_string(method) + " " + result.code + ": " +
                result.message;
        return false;
    }
    return sdv::decode_diagnostic_someip_response(result.value, method, output, error);
}

}  // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 30'496);
    constexpr std::uint16_t kClient = 0x0061U;
    sdv::DiagnosticWireResponse result;
    std::string error;

    if (!exchange(port, sdv::make_diagnostic_read_someip_request(kClient, 1U, true),
                  sdv::kReadDtcInformationSomeIpMethod, result, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    const auto initial_dtcs = result.dtcs.size();
    const bool records_valid = initial_dtcs == 10U && result.dtcs.front().confirmed &&
                               result.dtcs.front().occurrence_count == 2U;

    if (!exchange(port, sdv::make_diagnostic_clear_someip_request(kClient, 2U),
                  sdv::kClearDiagnosticInformationSomeIpMethod, result, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    const auto default_clear_status = result.status;

    if (!exchange(port,
                  sdv::make_diagnostic_session_someip_request(
                      kClient, 3U, sdv::DiagnosticSession::Extended),
                  sdv::kStartDiagnosticSessionSomeIpMethod, result, error) ||
        result.status != sdv::DiagnosticWireStatus::Ok) {
        std::cerr << (error.empty() ? "extended session rejected" : error) << '\n';
        return EXIT_FAILURE;
    }
    if (!exchange(port, sdv::make_diagnostic_seed_someip_request(kClient, 4U),
                  sdv::kRequestDiagnosticSeedSomeIpMethod, result, error) ||
        result.status != sdv::DiagnosticWireStatus::Ok) {
        std::cerr << (error.empty() ? "seed rejected" : error) << '\n';
        return EXIT_FAILURE;
    }
    const auto seed = result.seed;
    if (!exchange(port,
                  sdv::make_diagnostic_key_someip_request(
                      kClient, 5U, seed, sdv::DiagnosticManager::calculate_test_key(seed)),
                  sdv::kSubmitDiagnosticKeySomeIpMethod, result, error) ||
        result.status != sdv::DiagnosticWireStatus::Ok) {
        std::cerr << (error.empty() ? "key rejected" : error) << '\n';
        return EXIT_FAILURE;
    }
    if (!exchange(port, sdv::make_diagnostic_clear_someip_request(kClient, 6U),
                  sdv::kClearDiagnosticInformationSomeIpMethod, result, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    const auto authorized_clear_status = result.status;
    if (!exchange(port, sdv::make_diagnostic_read_someip_request(kClient, 7U, true),
                  sdv::kReadDtcInformationSomeIpMethod, result, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    const auto final_dtcs = result.dtcs.size();

    std::cout << "{\"client_id\":" << kClient
              << ",\"requests\":7,\"initial_dtcs\":" << initial_dtcs
              << ",\"records_valid\":" << (records_valid ? "true" : "false")
              << ",\"default_clear_status\":"
              << static_cast<unsigned int>(default_clear_status)
              << ",\"seed_nonzero\":" << (seed != 0U ? "true" : "false")
              << ",\"authorized_clear_status\":"
              << static_cast<unsigned int>(authorized_clear_status)
              << ",\"final_dtcs\":" << final_dtcs << "}\n";

    return records_valid &&
                   default_clear_status == sdv::DiagnosticWireStatus::SessionDenied &&
                   seed != 0U && authorized_clear_status == sdv::DiagnosticWireStatus::Ok &&
                   final_dtcs == 0U
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "sdv/gateway_diagnostic_bridge.hpp"
#include "sdv/vehicle_service_runtime.hpp"
#include "sdv/vehicle_someip_adapter.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace {

sdv::CanFrame frame(std::uint32_t id, std::array<std::uint8_t, 8> data,
                    std::int64_t time_ms, std::string trace, std::uint8_t dlc = 8U) {
    return {id, dlc, data, time_ms, std::move(trace)};
}

bool observe(sdv::VehicleGateway& gateway, sdv::GatewayDiagnosticBridge& bridge,
             const sdv::CanFrame& value) {
    return bridge.observe_ingest(value, gateway.ingest(value));
}

bool run_fault_cycle(sdv::VehicleGateway& gateway, sdv::GatewayDiagnosticBridge& bridge,
                     std::uint64_t cycle, std::int64_t base) {
    if (!bridge.begin_cycle(cycle, base, "evidence-cycle").accepted) {
        return false;
    }
    const std::array<sdv::CanFrame, 8> frames{
        frame(sdv::VehicleGateway::kVehicleStatusId,
              {0x10, 0x27, 80, 0, 0, 0, 0, 0}, base, "evidence-vcu"),
        frame(sdv::VehicleGateway::kTirePressureId,
              {230, 0, 231, 0, 229, 0, 232, 0}, base + 1, "evidence-tpms"),
        frame(sdv::VehicleGateway::kBodyStatusId,
              {0, 15, 1, 0, 0, 0, 0, 0}, base + 2, "evidence-bcm"),
        frame(sdv::VehicleGateway::kTboxHealthId,
              {1, 88, 0, 0, 0, 0, 0, 0}, base + 3, "evidence-tbox"),
        frame(sdv::VehicleGateway::kBodyStatusId, {}, base + 10, "evidence-dlc", 7U),
        frame(sdv::VehicleGateway::kVehicleStatusId,
              {0x0C, 0x62, 80, 0, 0, 0, 0, 0}, base + 11, "evidence-range"),
        frame(sdv::VehicleGateway::kTirePressureId,
              {230, 0, 231, 0, 229, 0, 232, 0}, base, "evidence-stale"),
        frame(0x777U, {}, base + 12, "evidence-unknown")};
    for (const auto& value : frames) {
        if (!observe(gateway, bridge, value)) {
            return false;
        }
    }
    const auto closed = bridge.close_cycle(base + 500, 100, "evidence-close");
    return closed.accepted && closed.failed_monitors == 8U;
}

}  // namespace

int main() {
    sdv::VehicleGateway gateway;
    sdv::DiagnosticManager diagnostics;
    sdv::GatewayDiagnosticBridge bridge(gateway, diagnostics);
    if (!bridge.initialize() || !run_fault_cycle(gateway, bridge, 1U, 1'000) ||
        !run_fault_cycle(gateway, bridge, 2U, 2'000)) {
        return EXIT_FAILURE;
    }

    const auto dtcs = diagnostics.active_dtcs();
    std::size_t confirmed = 0U;
    std::size_t freeze_frames = 0U;
    std::size_t occurrence_two = 0U;
    std::set<std::string> source_traces;
    for (const auto& dtc : dtcs) {
        confirmed += dtc.confirmed ? 1U : 0U;
        freeze_frames += dtc.freeze_frame.valid ? 1U : 0U;
        occurrence_two += dtc.occurrence_count == 2U ? 1U : 0U;
        source_traces.insert(dtc.last_trace_id);
    }

    std::int64_t now = 3'000;
    sdv::VehicleServiceRuntime runtime([&now] { return now; });
    if (!sdv::register_vehicle_diagnostic_service(runtime, diagnostics,
                                                  [&now] { return now; })) {
        return EXIT_FAILURE;
    }
    const sdv::ServiceRequest typed_request{
        "bridge-typed-read", "bridge-typed-trace", "bridge-evidence-reader",
        "vehicle.diagnostics", "ReadDTCInformation", 4'000, {"diagnostic.read"},
        sdv::ReadDtcInformationRequest{true}};
    const auto typed_response = runtime.invoke(typed_request);
    const auto* typed_dtcs =
        std::get_if<sdv::DiagnosticDtcListResult>(&typed_response.payload);
    const auto typed_count = typed_dtcs == nullptr ? 0U : typed_dtcs->dtcs.size();

    const auto wire_response = sdv::handle_vehicle_someip_request(
        sdv::make_diagnostic_read_someip_request(0x62U, 1U, true), runtime);
    sdv::DiagnosticWireResponse wire;
    std::string wire_error;
    const bool wire_decoded = sdv::decode_diagnostic_someip_response(
        wire_response, sdv::kReadDtcInformationSomeIpMethod, wire, wire_error);

    std::cout << "{\"registered_monitors\":8,\"fault_cycles\":2,\"active_dtcs\":"
              << dtcs.size() << ",\"confirmed_dtcs\":" << confirmed
              << ",\"freeze_frames\":" << freeze_frames
              << ",\"occurrence_count_two\":" << occurrence_two
              << ",\"distinct_source_traces\":" << source_traces.size()
              << ",\"typed_service_dtcs\":" << typed_count
              << ",\"wire_adapter_dtcs\":" << wire.dtcs.size()
              << ",\"wire_decoded\":" << (wire_decoded ? "true" : "false")
              << ",\"gateway_trace_events\":" << gateway.trace().size()
              << ",\"diagnostic_trace_events\":" << diagnostics.trace_snapshot().size()
              << ",\"runtime_trace_events\":" << runtime.trace_snapshot().size() << "}\n";

    return dtcs.size() == 8U && confirmed == 8U && freeze_frames == 8U &&
                   occurrence_two == 8U && source_traces.size() == 8U &&
                   typed_response.accepted && typed_count == 8U && wire_decoded &&
                   wire.status == sdv::DiagnosticWireStatus::Ok && wire.dtcs.size() == 8U
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

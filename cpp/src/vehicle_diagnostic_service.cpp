#include "sdv/vehicle_service_runtime.hpp"

#include <utility>

namespace sdv {

bool register_vehicle_diagnostic_service(VehicleServiceRuntime& runtime,
                                         DiagnosticManager& diagnostics,
                                         VehicleServiceRuntime::Clock clock) {
    if (!clock) {
        return false;
    }
    ServiceDescriptor descriptor{
        "vehicle.diagnostics",
        "1.0.0",
        {{"StartSession", "diagnostic.session", false},
         {"RequestSecuritySeed", "diagnostic.security", false},
         {"SubmitSecurityKey", "diagnostic.security", false},
         {"ReadDTCInformation", "diagnostic.read", false},
         {"ClearDiagnosticInformation", "diagnostic.clear", true}}};
    return runtime.register_service(
        std::move(descriptor),
        [&diagnostics, clock = std::move(clock)](const ServiceRequest& request) -> ServiceResponse {
            const auto now_ms = clock();
            DiagnosticOutcome outcome;
            ServiceResponsePayload payload = std::monostate{};
            if (request.method == "StartSession") {
                const auto* command =
                    std::get_if<StartDiagnosticSessionRequest>(&request.payload);
                if (command == nullptr) {
                    return {false, "INVALID_PAYLOAD",
                            "StartSession requires StartDiagnosticSessionRequest",
                            std::monostate{}};
                }
                outcome = diagnostics.start_session(request.principal, command->session, now_ms,
                                                    request.trace_id);
            } else if (request.method == "RequestSecuritySeed") {
                if (!std::holds_alternative<RequestDiagnosticSeedRequest>(request.payload)) {
                    return {false, "INVALID_PAYLOAD",
                            "RequestSecuritySeed requires RequestDiagnosticSeedRequest",
                            std::monostate{}};
                }
                outcome = diagnostics.request_security_seed(request.principal, now_ms,
                                                            request.trace_id);
                if (outcome.accepted) {
                    payload = DiagnosticSeedResult{outcome.seed};
                }
            } else if (request.method == "SubmitSecurityKey") {
                const auto* command =
                    std::get_if<SubmitDiagnosticKeyRequest>(&request.payload);
                if (command == nullptr) {
                    return {false, "INVALID_PAYLOAD",
                            "SubmitSecurityKey requires SubmitDiagnosticKeyRequest",
                            std::monostate{}};
                }
                outcome = diagnostics.submit_security_key(
                    request.principal, command->seed, command->key, now_ms, request.trace_id);
            } else if (request.method == "ReadDTCInformation") {
                const auto* query = std::get_if<ReadDtcInformationRequest>(&request.payload);
                if (query == nullptr) {
                    return {false, "INVALID_PAYLOAD",
                            "ReadDTCInformation requires ReadDtcInformationRequest",
                            std::monostate{}};
                }
                payload = DiagnosticDtcListResult{
                    query->active_only ? diagnostics.active_dtcs() : diagnostics.all_dtcs()};
                outcome = {true, "OK", "DTC information returned", 0U};
            } else if (request.method == "ClearDiagnosticInformation") {
                if (!std::holds_alternative<ClearDiagnosticInformationRequest>(request.payload)) {
                    return {false, "INVALID_PAYLOAD",
                            "ClearDiagnosticInformation requires ClearDiagnosticInformationRequest",
                            std::monostate{}};
                }
                outcome = diagnostics.clear_all(request.principal, now_ms, request.trace_id);
            } else {
                return {false, "METHOD_NOT_FOUND", "diagnostic method is not implemented",
                        std::monostate{}};
            }
            return {outcome.accepted, std::move(outcome.code), std::move(outcome.message),
                    std::move(payload)};
        });
}

}  // namespace sdv

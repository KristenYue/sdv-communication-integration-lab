#include "sdv/vehicle_service_runtime.hpp"

#include "sdv/idempotency_journal.hpp"
#include "sdv/vehicle_gateway.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sdv {

VehicleServiceRuntime::VehicleServiceRuntime(Clock clock, std::string journal_path)
    : clock_(std::move(clock)) {
    if (!clock_) {
        throw std::invalid_argument("vehicle service runtime requires a clock");
    }
    if (!journal_path.empty()) {
        journal_ = std::make_unique<IdempotencyJournal>(std::move(journal_path));
        std::vector<RecoveredJournalRequest> recovered;
        std::string error;
        if (!journal_->recover(recovered, error)) {
            throw std::runtime_error("idempotency journal recovery failed: " + error);
        }
        for (auto& entry : recovered) {
            completed_requests_.emplace(
                std::move(entry.request_id),
                CompletedRequest{std::move(entry.fingerprint), std::move(entry.response)});
        }
    }
}

VehicleServiceRuntime::~VehicleServiceRuntime() = default;

bool VehicleServiceRuntime::register_service(ServiceDescriptor descriptor, Handler handler) {
    if (descriptor.name.empty() || descriptor.version.empty() || descriptor.methods.empty() ||
        !handler || services_.contains(descriptor.name)) {
        return false;
    }
    std::set<std::string> method_names;
    for (const auto& method : descriptor.methods) {
        if (method.name.empty() || !method_names.insert(method.name).second) {
            return false;
        }
    }
    const auto name = descriptor.name;
    services_.emplace(name, RegisteredService{std::move(descriptor), std::move(handler)});
    return true;
}

std::vector<ServiceDescriptor> VehicleServiceRuntime::discover() const {
    std::vector<ServiceDescriptor> result;
    result.reserve(services_.size());
    for (const auto& [name, service] : services_) {
        static_cast<void>(name);
        result.push_back(service.descriptor);
    }
    return result;
}

void VehicleServiceRuntime::record(const ServiceRequest& request, std::string event,
                                   std::string detail) {
    trace_.push_back(ServiceRuntimeEvent{next_sequence_++, clock_(), request.request_id,
                                         request.trace_id, request.service, request.method,
                                         std::move(event), std::move(detail)});
}

ServiceResponse VehicleServiceRuntime::reject(const ServiceRequest& request, std::string code,
                                              std::string message) {
    record(request, code, message);
    return ServiceResponse{false, std::move(code), std::move(message), std::monostate{}};
}

std::string VehicleServiceRuntime::fingerprint(const ServiceRequest& request) {
    std::ostringstream output;
    output << request.principal << '|' << request.service << '|' << request.method << '|';
    if (std::holds_alternative<std::monostate>(request.payload)) {
        output << "none";
    } else if (const auto* query = std::get_if<GetVehicleSnapshotRequest>(&request.payload)) {
        output << "snapshot:" << query->now_ms << ':' << query->max_age_ms;
    } else {
        const auto& command = std::get<SetDoorLocksRequest>(request.payload);
        output << "door-locks:" << static_cast<unsigned int>(command.door_mask) << ':'
               << (command.lock ? '1' : '0');
    }
    return output.str();
}

ServiceResponse VehicleServiceRuntime::complete(const ServiceRequest& request,
                                                const std::string& request_fingerprint,
                                                ServiceResponse response, bool persist) {
    if (persist && journal_ != nullptr) {
        std::string error;
        if (!journal_->append_complete(request.request_id, request_fingerprint, response, error)) {
            response = reject(request, "COMPLETION_UNKNOWN",
                              "terminal journal write failed after dispatch: " + error);
        }
    }
    completed_requests_.emplace(
        request.request_id,
        CompletedRequest{request_fingerprint, response});
    return response;
}

ServiceResponse VehicleServiceRuntime::invoke(const ServiceRequest& request) {
    if (request.request_id.empty() || request.request_id.size() > 64U) {
        return reject(request, "INVALID_REQUEST_ID", "request_id must contain 1..64 characters");
    }
    if (request.trace_id.empty() || request.trace_id.size() > 64U) {
        return reject(request, "INVALID_TRACE_ID", "trace_id must contain 1..64 characters");
    }
    if (request.principal.empty() || request.principal.size() > 64U) {
        return reject(request, "INVALID_PRINCIPAL", "principal must contain 1..64 characters");
    }
    const auto service = services_.find(request.service);
    if (service == services_.end()) {
        return reject(request, "SERVICE_NOT_FOUND", "requested service is not registered");
    }
    const auto& methods = service->second.descriptor.methods;
    const auto method = std::find_if(methods.begin(), methods.end(), [&](const auto& candidate) {
        return candidate.name == request.method;
    });
    if (method == methods.end()) {
        return reject(request, "METHOD_NOT_FOUND", "requested method is not registered");
    }
    if (!method->required_permission.empty() &&
        !request.permissions.contains(method->required_permission)) {
        return reject(request, "PERMISSION_DENIED", "principal lacks required permission");
    }
    const auto request_fingerprint = fingerprint(request);
    if (const auto cached = completed_requests_.find(request.request_id);
        cached != completed_requests_.end()) {
        if (cached->second.fingerprint != request_fingerprint) {
            return reject(request, "REQUEST_ID_CONFLICT",
                          "request_id was already used for different semantic content");
        }
        record(request, "IDEMPOTENT_REPLAY", "cached terminal response returned");
        return cached->second.response;
    }
    if (cancelled_requests_.contains(request.request_id)) {
        return reject(request, "CANCELLED", "request was cancelled before dispatch");
    }
    if (request.deadline_ms > 0 && clock_() > request.deadline_ms) {
        return reject(request, "DEADLINE_EXCEEDED", "deadline expired before dispatch");
    }

    const bool persist = method->state_changing && journal_ != nullptr;
    if (persist) {
        std::string error;
        if (!journal_->append_begin(request.request_id, request_fingerprint, error)) {
            return reject(request, "JOURNAL_ERROR",
                          "request was not dispatched because BEGIN could not be persisted: " +
                              error);
        }
    }

    record(request, "DISPATCHED", "request passed runtime admission checks");
    ServiceResponse response;
    try {
        response = service->second.handler(request);
    } catch (const std::exception& error) {
        response = reject(request, "COMPLETION_UNKNOWN",
                          std::string("handler failed after dispatch: ") + error.what());
        return complete(request, request_fingerprint, std::move(response), persist);
    } catch (...) {
        response = reject(request, "COMPLETION_UNKNOWN",
                          "handler failed after dispatch with a non-standard exception");
        return complete(request, request_fingerprint, std::move(response), persist);
    }

    if (cancelled_requests_.contains(request.request_id)) {
        response = reject(request, "COMPLETION_UNKNOWN",
                          "request was cancelled after dispatch; side effects may have occurred");
        return complete(request, request_fingerprint, std::move(response), persist);
    }
    if (request.deadline_ms > 0 && clock_() > request.deadline_ms) {
        response = reject(request, "COMPLETION_UNKNOWN",
                          "deadline expired after dispatch; side effects may have occurred");
        return complete(request, request_fingerprint, std::move(response), persist);
    }
    record(request, response.code, response.message);
    return complete(request, request_fingerprint, std::move(response), persist);
}

bool VehicleServiceRuntime::cancel(const std::string& request_id) {
    if (request_id.empty() || request_id.size() > 64U) {
        return false;
    }
    return cancelled_requests_.insert(request_id).second;
}

bool register_vehicle_state_service(VehicleServiceRuntime& runtime,
                                    const VehicleGateway& gateway) {
    ServiceDescriptor descriptor{
        "vehicle.state", "1.0.0", {{"GetSnapshot", "vehicle.state.read"}}};
    return runtime.register_service(
        std::move(descriptor), [&gateway](const ServiceRequest& request) -> ServiceResponse {
            const auto* query = std::get_if<GetVehicleSnapshotRequest>(&request.payload);
            if (query == nullptr) {
                return ServiceResponse{false, "INVALID_PAYLOAD",
                                       "GetSnapshot requires GetVehicleSnapshotRequest",
                                       std::monostate{}};
            }
            return ServiceResponse{true, "OK", "vehicle snapshot returned",
                                   gateway.snapshot(query->now_ms, query->max_age_ms)};
        });
}

}  // namespace sdv

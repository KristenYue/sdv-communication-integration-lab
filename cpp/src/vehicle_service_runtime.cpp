#include "sdv/vehicle_service_runtime.hpp"

#include "sdv/vehicle_gateway.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace sdv {

VehicleServiceRuntime::VehicleServiceRuntime(Clock clock) : clock_(std::move(clock)) {
    if (!clock_) {
        throw std::invalid_argument("vehicle service runtime requires a clock");
    }
}

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
    if (cancelled_requests_.contains(request.request_id)) {
        return reject(request, "CANCELLED", "request was cancelled before dispatch");
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
    if (request.deadline_ms > 0 && clock_() > request.deadline_ms) {
        return reject(request, "DEADLINE_EXCEEDED", "deadline expired before dispatch");
    }

    record(request, "DISPATCHED", "request passed runtime admission checks");
    ServiceResponse response;
    try {
        response = service->second.handler(request);
    } catch (const std::exception& error) {
        return reject(request, "HANDLER_ERROR", error.what());
    } catch (...) {
        return reject(request, "HANDLER_ERROR", "handler raised a non-standard exception");
    }

    if (cancelled_requests_.contains(request.request_id)) {
        return reject(request, "COMPLETION_UNKNOWN",
                      "request was cancelled after dispatch; side effects may have occurred");
    }
    if (request.deadline_ms > 0 && clock_() > request.deadline_ms) {
        return reject(request, "COMPLETION_UNKNOWN",
                      "deadline expired after dispatch; side effects may have occurred");
    }
    record(request, response.code, response.message);
    return response;
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

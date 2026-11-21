#include "sdv/vehicle_service_runtime.hpp"

#include "sdv/idempotency_journal.hpp"
#include "sdv/vehicle_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sdv {

VehicleServiceRuntime::VehicleServiceRuntime(Clock clock, std::string journal_path,
                                             std::size_t max_in_flight)
    : clock_(std::move(clock)), max_in_flight_(max_in_flight) {
    if (!clock_) {
        throw std::invalid_argument("vehicle service runtime requires a clock");
    }
    if (max_in_flight_ == 0U) {
        throw std::invalid_argument("vehicle service runtime requires max_in_flight > 0");
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

std::vector<ServiceRuntimeEvent> VehicleServiceRuntime::trace_snapshot() const {
    std::lock_guard lock(trace_mutex_);
    return trace_;
}

bool VehicleServiceRuntime::register_service(ServiceDescriptor descriptor, Handler handler) {
    std::lock_guard lock(state_mutex_);
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
    std::lock_guard lock(state_mutex_);
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
    std::lock_guard lock(trace_mutex_);
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
    } else if (const auto* command = std::get_if<SetDoorLocksRequest>(&request.payload)) {
        output << "door-locks:" << static_cast<unsigned int>(command->door_mask) << ':'
               << (command->lock ? '1' : '0');
    } else if (const auto* telemetry = std::get_if<TelemetryUploadRequest>(&request.payload)) {
        output << "telemetry:" << telemetry->node_id << ':' << telemetry->payload.size() << ':';
        for (const auto byte : telemetry->payload) {
            output << static_cast<char>("0123456789abcdef"[byte >> 4U])
                   << static_cast<char>("0123456789abcdef"[byte & 0x0FU]);
        }
    } else if (const auto* session =
                   std::get_if<StartDiagnosticSessionRequest>(&request.payload)) {
        output << "diagnostic-session:"
               << (session->session == DiagnosticSession::Extended ? "extended" : "default");
    } else if (std::holds_alternative<RequestDiagnosticSeedRequest>(request.payload)) {
        output << "diagnostic-seed";
    } else if (const auto* key =
                   std::get_if<SubmitDiagnosticKeyRequest>(&request.payload)) {
        output << "diagnostic-key:" << key->seed << ':' << key->key;
    } else if (const auto* read =
                   std::get_if<ReadDtcInformationRequest>(&request.payload)) {
        output << "diagnostic-read:" << (read->active_only ? '1' : '0');
    } else {
        output << "diagnostic-clear";
    }
    return output.str();
}

ServiceResponse VehicleServiceRuntime::complete(const ServiceRequest& request,
                                                const std::string& request_fingerprint,
                                                ServiceResponse response, bool persist) {
    if (persist && journal_ != nullptr) {
        std::string error;
        std::lock_guard journal_lock(journal_mutex_);
        if (!journal_->append_complete(request.request_id, request_fingerprint, response, error)) {
            response = reject(request, "COMPLETION_UNKNOWN",
                              "terminal journal write failed after dispatch: " + error);
        }
    }
    {
        std::lock_guard lock(state_mutex_);
        completed_requests_.emplace(
            request.request_id,
            CompletedRequest{request_fingerprint, response});
        in_flight_requests_.erase(request.request_id);
        if (active_handlers_ > 0U) {
            --active_handlers_;
        }
    }
    state_changed_.notify_all();
    return response;
}

void VehicleServiceRuntime::release_in_flight(const std::string& request_id) {
    {
        std::lock_guard lock(state_mutex_);
        in_flight_requests_.erase(request_id);
        if (active_handlers_ > 0U) {
            --active_handlers_;
        }
    }
    state_changed_.notify_all();
}

bool VehicleServiceRuntime::is_cancelled(const std::string& request_id) const {
    std::lock_guard lock(state_mutex_);
    return cancelled_requests_.contains(request_id);
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

    Handler handler;
    ServiceMethodDescriptor method_descriptor;
    bool service_found = false;
    bool method_found = false;
    {
        std::lock_guard lock(state_mutex_);
        const auto service = services_.find(request.service);
        if (service != services_.end()) {
            service_found = true;
            const auto& methods = service->second.descriptor.methods;
            const auto method =
                std::find_if(methods.begin(), methods.end(), [&](const auto& candidate) {
                    return candidate.name == request.method;
                });
            if (method != methods.end()) {
                method_found = true;
                handler = service->second.handler;
                method_descriptor = *method;
            }
        }
    }
    if (!service_found) {
        return reject(request, "SERVICE_NOT_FOUND", "requested service is not registered");
    }
    if (!method_found) {
        return reject(request, "METHOD_NOT_FOUND", "requested method is not registered");
    }
    if (!method_descriptor.required_permission.empty() &&
        !request.permissions.contains(method_descriptor.required_permission)) {
        return reject(request, "PERMISSION_DENIED", "principal lacks required permission");
    }

    const auto request_fingerprint = fingerprint(request);
    while (true) {
        std::unique_lock lock(state_mutex_);
        if (const auto cached = completed_requests_.find(request.request_id);
            cached != completed_requests_.end()) {
            if (cached->second.fingerprint != request_fingerprint) {
                lock.unlock();
                return reject(request, "REQUEST_ID_CONFLICT",
                              "request_id was already used for different semantic content");
            }
            const auto response = cached->second.response;
            lock.unlock();
            record(request, "IDEMPOTENT_REPLAY", "cached terminal response returned");
            return response;
        }

        if (cancelled_requests_.contains(request.request_id)) {
            lock.unlock();
            return reject(request, "CANCELLED", "request was cancelled before dispatch");
        }

        if (const auto in_flight = in_flight_requests_.find(request.request_id);
            in_flight != in_flight_requests_.end()) {
            if (in_flight->second.fingerprint != request_fingerprint) {
                lock.unlock();
                return reject(request, "REQUEST_ID_CONFLICT",
                              "request_id is in flight with different semantic content");
            }
            lock.unlock();
            record(request, "IN_FLIGHT_WAIT", "identical request is already executing");
            lock.lock();
            const auto current = in_flight_requests_.find(request.request_id);
            if (current == in_flight_requests_.end()) {
                continue;
            }
            if (current->second.fingerprint != request_fingerprint) {
                lock.unlock();
                return reject(request, "REQUEST_ID_CONFLICT",
                              "request_id changed semantic content while waiting");
            }
            if (request.deadline_ms == 0) {
                state_changed_.wait(lock, [&] {
                    return !in_flight_requests_.contains(request.request_id) ||
                           cancelled_requests_.contains(request.request_id);
                });
            } else {
                const auto remaining = request.deadline_ms - clock_();
                if (remaining <= 0 ||
                    !state_changed_.wait_for(lock, std::chrono::milliseconds(remaining), [&] {
                        return !in_flight_requests_.contains(request.request_id) ||
                               cancelled_requests_.contains(request.request_id);
                    })) {
                    lock.unlock();
                    return reject(request, "QUEUE_TIMEOUT",
                                  "deadline expired while waiting for identical in-flight request");
                }
            }
            continue;
        }

        if (request.deadline_ms > 0 && clock_() > request.deadline_ms) {
            lock.unlock();
            return reject(request, "DEADLINE_EXCEEDED", "deadline expired before dispatch");
        }

        if (active_handlers_ >= max_in_flight_) {
            if (request.deadline_ms == 0) {
                lock.unlock();
                return reject(request, "QUEUE_FULL",
                              "runtime is at capacity and request has no queue deadline");
            }
            lock.unlock();
            record(request, "CAPACITY_WAIT", "request is waiting for a handler slot");
            lock.lock();
            if (active_handlers_ < max_in_flight_) {
                continue;
            }
            const auto remaining = request.deadline_ms - clock_();
            if (remaining <= 0 ||
                !state_changed_.wait_for(lock, std::chrono::milliseconds(remaining), [&] {
                    return active_handlers_ < max_in_flight_ ||
                           cancelled_requests_.contains(request.request_id);
                })) {
                lock.unlock();
                return reject(request, "QUEUE_TIMEOUT",
                              "deadline expired while waiting for runtime capacity");
            }
            continue;
        }

        in_flight_requests_.emplace(request.request_id,
                                    InFlightRequest{request_fingerprint});
        ++active_handlers_;
        break;
    }

    const bool persist = method_descriptor.state_changing && journal_ != nullptr;
    if (persist) {
        std::string error;
        std::lock_guard journal_lock(journal_mutex_);
        if (!journal_->append_begin(request.request_id, request_fingerprint, error)) {
            release_in_flight(request.request_id);
            return reject(request, "JOURNAL_ERROR",
                          "request was not dispatched because BEGIN could not be persisted: " +
                              error);
        }
    }

    record(request, "DISPATCHED", "request passed runtime admission checks");
    ServiceResponse response;
    try {
        response = handler(request);
    } catch (const std::exception& error) {
        response = reject(request, "COMPLETION_UNKNOWN",
                          std::string("handler failed after dispatch: ") + error.what());
        return complete(request, request_fingerprint, std::move(response), persist);
    } catch (...) {
        response = reject(request, "COMPLETION_UNKNOWN",
                          "handler failed after dispatch with a non-standard exception");
        return complete(request, request_fingerprint, std::move(response), persist);
    }

    if (is_cancelled(request.request_id)) {
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
    bool inserted = false;
    {
        std::lock_guard lock(state_mutex_);
        inserted = cancelled_requests_.insert(request_id).second;
    }
    state_changed_.notify_all();
    return inserted;
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

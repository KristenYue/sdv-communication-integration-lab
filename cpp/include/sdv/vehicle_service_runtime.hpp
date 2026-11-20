#pragma once

#include "sdv/types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace sdv {

class VehicleGateway;

struct GetVehicleSnapshotRequest {
    std::int64_t now_ms{0};
    std::int64_t max_age_ms{0};
};

struct SetDoorLocksRequest {
    std::uint8_t door_mask{0};
    bool lock{false};
};

struct DoorLockResult {
    std::uint8_t requested_mask{0};
    bool locked{false};
};

using ServiceRequestPayload =
    std::variant<std::monostate, GetVehicleSnapshotRequest, SetDoorLocksRequest>;
using ServiceResponsePayload = std::variant<std::monostate, VehicleSnapshot, DoorLockResult>;

struct ServiceMethodDescriptor {
    std::string name;
    std::string required_permission;
};

struct ServiceDescriptor {
    std::string name;
    std::string version;
    std::vector<ServiceMethodDescriptor> methods;
};

struct ServiceRequest {
    std::string request_id;
    std::string trace_id;
    std::string principal;
    std::string service;
    std::string method;
    std::int64_t deadline_ms{0};
    std::set<std::string> permissions;
    ServiceRequestPayload payload;
};

struct ServiceResponse {
    bool accepted{false};
    std::string code;
    std::string message;
    ServiceResponsePayload payload;
};

struct ServiceRuntimeEvent {
    std::uint64_t sequence{0};
    std::int64_t time_ms{0};
    std::string request_id;
    std::string trace_id;
    std::string service;
    std::string method;
    std::string event;
    std::string detail;
};

class VehicleServiceRuntime {
public:
    using Clock = std::function<std::int64_t()>;
    using Handler = std::function<ServiceResponse(const ServiceRequest&)>;

    explicit VehicleServiceRuntime(Clock clock);

    bool register_service(ServiceDescriptor descriptor, Handler handler);
    std::vector<ServiceDescriptor> discover() const;
    ServiceResponse invoke(const ServiceRequest& request);
    bool cancel(const std::string& request_id);

    const std::vector<ServiceRuntimeEvent>& trace() const noexcept { return trace_; }

private:
    struct RegisteredService {
        ServiceDescriptor descriptor;
        Handler handler;
    };

    Clock clock_;
    std::map<std::string, RegisteredService> services_;
    std::set<std::string> cancelled_requests_;
    std::vector<ServiceRuntimeEvent> trace_;
    std::uint64_t next_sequence_{1};

    void record(const ServiceRequest& request, std::string event, std::string detail);
    ServiceResponse reject(const ServiceRequest& request, std::string code,
                           std::string message);
};

bool register_vehicle_state_service(VehicleServiceRuntime& runtime,
                                    const VehicleGateway& gateway);

}  // namespace sdv

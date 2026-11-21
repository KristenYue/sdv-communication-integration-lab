#pragma once

#include "sdv/diagnostic_manager.hpp"
#include "sdv/types.hpp"

#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace sdv {

class VehicleGateway;
class IdempotencyJournal;

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

struct TelemetryUploadRequest {
    std::uint32_t node_id{0};
    std::vector<std::uint8_t> payload;
};

struct TelemetryUploadResult {
    std::size_t accepted_bytes{0};
};

struct StartDiagnosticSessionRequest {
    DiagnosticSession session{DiagnosticSession::Default};
};

struct RequestDiagnosticSeedRequest {};

struct SubmitDiagnosticKeyRequest {
    std::uint32_t seed{0};
    std::uint32_t key{0};
};

struct ReadDtcInformationRequest {
    bool active_only{true};
};

struct ClearDiagnosticInformationRequest {};

struct DiagnosticSeedResult {
    std::uint32_t seed{0};
};

struct DiagnosticDtcListResult {
    std::vector<DiagnosticTroubleCode> dtcs;
};

using ServiceRequestPayload =
    std::variant<std::monostate, GetVehicleSnapshotRequest, SetDoorLocksRequest,
                 TelemetryUploadRequest, StartDiagnosticSessionRequest,
                 RequestDiagnosticSeedRequest, SubmitDiagnosticKeyRequest,
                 ReadDtcInformationRequest, ClearDiagnosticInformationRequest>;
using ServiceResponsePayload =
    std::variant<std::monostate, VehicleSnapshot, DoorLockResult, TelemetryUploadResult,
                 DiagnosticSeedResult, DiagnosticDtcListResult>;

struct ServiceMethodDescriptor {
    std::string name;
    std::string required_permission;
    bool state_changing{false};
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

    explicit VehicleServiceRuntime(Clock clock, std::string journal_path = {},
                                   std::size_t max_in_flight = 64U);
    ~VehicleServiceRuntime();

    bool register_service(ServiceDescriptor descriptor, Handler handler);
    std::vector<ServiceDescriptor> discover() const;
    ServiceResponse invoke(const ServiceRequest& request);
    bool cancel(const std::string& request_id);

    std::vector<ServiceRuntimeEvent> trace_snapshot() const;

private:
    struct RegisteredService {
        ServiceDescriptor descriptor;
        Handler handler;
    };

    struct CompletedRequest {
        std::string fingerprint;
        ServiceResponse response;
    };

    struct InFlightRequest {
        std::string fingerprint;
    };

    Clock clock_;
    std::map<std::string, RegisteredService> services_;
    std::map<std::string, CompletedRequest> completed_requests_;
    std::map<std::string, InFlightRequest> in_flight_requests_;
    std::unique_ptr<IdempotencyJournal> journal_;
    std::set<std::string> cancelled_requests_;
    std::vector<ServiceRuntimeEvent> trace_;
    std::uint64_t next_sequence_{1};
    std::size_t active_handlers_{0};
    std::size_t max_in_flight_{64U};
    mutable std::mutex state_mutex_;
    mutable std::mutex trace_mutex_;
    mutable std::mutex journal_mutex_;
    std::condition_variable state_changed_;

    void record(const ServiceRequest& request, std::string event, std::string detail);
    ServiceResponse reject(const ServiceRequest& request, std::string code,
                           std::string message);
    static std::string fingerprint(const ServiceRequest& request);
    ServiceResponse complete(const ServiceRequest& request, const std::string& fingerprint,
                             ServiceResponse response, bool persist);
    void release_in_flight(const std::string& request_id);
    bool is_cancelled(const std::string& request_id) const;
};

bool register_vehicle_state_service(VehicleServiceRuntime& runtime,
                                    const VehicleGateway& gateway);
bool register_vehicle_diagnostic_service(VehicleServiceRuntime& runtime,
                                         DiagnosticManager& diagnostics,
                                         VehicleServiceRuntime::Clock clock);

}  // namespace sdv

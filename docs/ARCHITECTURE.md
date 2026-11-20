# Vehicle service runtime architecture and evidence boundaries

The platform is organized around production-code ownership rather than test execution. The C++ communication gateway and the upcoming vehicle-service runtime are the product under development. Frozen scenarios, fault injection and replay are evidence mechanisms around that product. AI or diagnostic agents may consume the service API later, but they do not define the core architecture.

## Current executable paths

```text
Path A - deterministic contract and multi-node integration

VCU process ----\
TPMS process ----+--> multiprocessing control queue --> Python controller
BCM process -----+                                      |
T-Box process ---/                                      v
                                                 C++ gateway process
                                                        |
                                                        +--> signal state
                                                        +--> freshness
                                                        +--> trace/result

Path B - Linux transport acceptance

Python virtual ECU sender --> Linux vcan0 --> C++ SocketCAN gateway
                                             |
                                             +--> same C++ gateway core
                                             +--> JSON evidence

Path C - vehicle service runtime

C++ VehicleGateway --> typed vehicle snapshot --> C++ VehicleServiceRuntime
                                                    |
                                                    +--> registry / discovery
                                                    +--> permission admission
                                                    +--> deadline / cancellation semantics
                                                    +--> request and trace events
                                                    +--> diagnostic, cloud or optional AI consumer
```

Path A exists to exercise process lifecycle, multiple signal owners, restarts, correlation IDs and deterministic scenario assertions on both Windows and Linux. Its queue is orchestration infrastructure, not an automotive network.

Path B exists to prove that the C++ data plane can receive native Linux CAN frames. `vcan0` is a kernel virtual CAN interface, not a physical CAN controller, vehicle harness or production ECU.

Path C is the product-facing service abstraction. Its first adapter is read-only and in-process so the request contract and failure semantics can be established before adding a network transport. A timeout before dispatch means the handler did not run. A timeout or cancellation observed after dispatch returns `COMPLETION_UNKNOWN` because the runtime cannot honestly promise rollback of handler side effects.

Path D carries that read-only method across two C++ processes using a bounded SOME/IP header and localhost UDP. The server maps Service ID `0x1234` and Method ID `0x0001` into the same typed runtime call. This proves cross-process request correlation and dispatch, not Service Discovery or production-stack interoperability.

## Component ownership

| Component | Language | Responsibility |
|---|---|---|
| `VehicleGateway` | C++20 | CAN identifier/DLC/range/time contract, signal state, freshness and trace |
| `sdv_gateway_cli` | C++20 | Stable process boundary used by scenario and multi-node controllers |
| `sdv_socketcan_gateway` | C++20/Linux | Native `PF_CAN`/`CAN_RAW` receive path into the same gateway core |
| `VehicleServiceRuntime` | C++20 | Service registry, typed dispatch, permissions, deadlines, cancellation semantics and trace |
| `vehicle.state` adapter | C++20 | Read-only service boundary over the gateway snapshot |
| bounded SOME/IP codec | C++20 | Big-endian base-header encoding/decoding and malformed-message rejection |
| SOME/IP UDP server/client | C++20 | Cross-process localhost request/response into the vehicle-service runtime |

## Idempotency boundary

Completed runtime requests are indexed by Request ID and an exact semantic fingerprint over Principal, Service, Method and typed Payload. Service existence, method existence and the caller's current permission are checked before cache lookup, so replay cannot bypass authorization. An identical retry returns the cached typed response; a different fingerprint returns `REQUEST_ID_CONFLICT`. Retry-local Trace ID, permissions and Deadline do not change the operation identity. Post-dispatch uncertain outcomes are cached because repeating an operation whose side effects are unknown is unsafe.

The default store is in-memory and invocation is sequential. It is not described as concurrent idempotency because an in-flight coordination protocol has not yet been implemented.

State-changing methods may additionally use the append-only recovery journal. A flushed `BEGIN` precedes Handler dispatch and a `COMPLETE` follows it. Cleanly completed requests recover their typed response; an unmatched `BEGIN` recovers as `COMPLETION_UNKNOWN` and is not dispatched again. CRC or schema corruption fails startup closed. Detailed state transitions and durability boundaries are in [RECOVERY_SEMANTICS.md](RECOVERY_SEMANTICS.md).

Concurrent callers are coordinated by an in-flight Request ID table and condition variable. Identical followers wait and replay one terminal result; conflicting fingerprints are rejected. A configurable active-Handler bound provides `QUEUE_FULL` and Deadline-aware `QUEUE_TIMEOUT` backpressure. This is caller-thread admission control rather than an internal worker scheduler; details are in [CONCURRENCY_SEMANTICS.md](CONCURRENCY_SEMANTICS.md).

The bounded SOME/IP path also exposes a synthetic `vehicle.body/SetDoorLocks` method for recovery experiments. The fault harness may execute and journal the request while intentionally dropping the UDP response. A restarted server loads the same journal and replays the typed response for the same Client/Session-derived Request ID without invoking the Handler again. This models an uncertain network outcome over localhost, not a physical actuator transaction.

The performance path uses a separate typed `vehicle.telemetry/Upload` method and a fresh runtime per matrix cell. It measures in-process runtime work only; the transport paths remain outside those latency numbers. Methodology and the full boundary are in [PERFORMANCE_MATRIX.md](PERFORMANCE_MATRIX.md).
| frozen scenario controller | Python | Versioned input generation, expected results, hashes and reports |
| virtual ECU workers | Python processes | VCU/TPMS/BCM/T-Box signal ownership and restart generations |
| DBC subset decoder | Python | Independent contract check for the project-supported message subset |

## Current signal ownership

| CAN ID | Owner | Signals |
|---:|---|---|
| `0x100` | VCU | speed, battery, door flag |
| `0x101` | TPMS | four tire pressures |
| `0x102` | BCM | door-open mask, door-lock mask, wiper state |
| `0x103` | T-Box | cloud connectivity, signal quality, upload backlog |

## Claims that are not allowed yet

- physical CAN bus or vehicle integration;
- complete DBC support;
- AUTOSAR compliance;
- production SOME/IP or DDS interoperability;
- cross-process service discovery or transport for the vehicle-service runtime;
- power-loss durability, cross-process concurrent arbitration or real-time scheduling;
- deterministic real-time guarantees;
- production safety, cybersecurity or functional-safety certification;
- physical-vehicle control by an AI or diagnostic agent.

Those boundaries remain explicit until the corresponding source, environment record and reproducible result exist.

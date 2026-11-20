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
```

Path A exists to exercise process lifecycle, multiple signal owners, restarts, correlation IDs and deterministic scenario assertions on both Windows and Linux. Its queue is orchestration infrastructure, not an automotive network.

Path B exists to prove that the C++ data plane can receive native Linux CAN frames. `vcan0` is a kernel virtual CAN interface, not a physical CAN controller, vehicle harness or production ECU.

## Component ownership

| Component | Language | Responsibility |
|---|---|---|
| `VehicleGateway` | C++20 | CAN identifier/DLC/range/time contract, signal state, freshness and trace |
| `sdv_gateway_cli` | C++20 | Stable process boundary used by scenario and multi-node controllers |
| `sdv_socketcan_gateway` | C++20/Linux | Native `PF_CAN`/`CAN_RAW` receive path into the same gateway core |
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
- SOME/IP or DDS interoperability;
- deterministic real-time guarantees;
- production safety, cybersecurity or functional-safety certification;
- physical-vehicle control by an AI or diagnostic agent.

Those boundaries remain explicit until the corresponding source, environment record and reproducible result exist.

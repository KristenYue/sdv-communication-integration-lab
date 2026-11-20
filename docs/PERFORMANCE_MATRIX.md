# Vehicle-service runtime performance matrix

## Scope

The matrix measures the in-process C++ `VehicleServiceRuntime` path with a typed `vehicle.telemetry/Upload` service. It includes request construction, semantic fingerprinting, admission, Handler dispatch, terminal-response caching and trace recording. It does not include CAN, SocketCAN, UDP, SOME/IP, an ECU, storage or a physical network.

## Dimensions and method

- aggregate target frequency: 100, 500 and 1,000 requests/second;
- typed payload: 64, 256 and 1,024 bytes;
- concurrent logical nodes: 1, 4 and 8 caller threads;
- 27 total cells and 0.25 seconds of scheduled traffic per cell;
- a fresh runtime instance per cell to avoid cross-cell idempotency-cache growth;
- deterministic pacing by global sequence number, with aggregate frequency distributed across nodes;
- Windows executable compiled with MSVC C++20 and `/O2`;
- latency measured around `invoke()` with `steady_clock`;
- Windows process CPU measured from kernel plus user time via `GetProcessTimes`;
- resident memory measured from `WorkingSetSize` after each cell.

## Windows result

- 3,600/3,600 calls succeeded;
- minimum achieved/target throughput ratio: 95.63%;
- maximum cell p50/p95/p99: 181.9/326.5/389.0 microseconds;
- worst p99 cell: 1,000 Hz, 1,024-byte payload, 8 nodes;
- maximum measured process CPU: 85.56%;
- maximum measured resident memory: 7.58 MiB.

These are short synthetic cells intended to compare runtime dimensions and detect regressions. They are not long-duration capacity certification, hard real-time guarantees or production hardware results. Process CPU may vary with Windows scheduling and timer resolution; the checked-in JSON is the evidence source.

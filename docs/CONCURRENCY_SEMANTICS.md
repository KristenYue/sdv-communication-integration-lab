# Runtime concurrency and backpressure semantics

The vehicle-service runtime coordinates concurrent callers around Request ID, semantic fingerprint and a configurable `max_in_flight` handler limit.

## Concurrent duplicate handling

```text
first request_id + fingerprint
  -> reserve in-flight ownership
  -> execute Handler once
  -> publish terminal response

identical followers
  -> observe same in-flight fingerprint
  -> wait on condition variable
  -> replay published terminal response

same request_id, different fingerprint
  -> REQUEST_ID_CONFLICT
  -> never enter Handler
```

Service and method lookup plus current permission validation happen before completed or in-flight replay. Cancellation wakes waiting callers. Runtime state, trace sequencing and journal writes use separate synchronization domains; callers read trace through a locked snapshot rather than a mutable reference.

## Capacity and deadline behavior

`max_in_flight` bounds the number of active Handlers. When capacity is exhausted:

- a request without a Deadline fails immediately with `QUEUE_FULL`;
- a request with a Deadline waits for a slot;
- if no slot becomes available before the Deadline, it returns `QUEUE_TIMEOUT` and never enters the Handler.

This is bounded admission and backpressure, not a priority scheduler. Handlers execute on caller threads; there is no internal worker pool, fairness guarantee, priority inversion control, CPU affinity or real-time scheduling claim.

## Measured boundary

The current evidence uses synthetic in-process Handlers on Windows: 32 concurrent identical callers, 16 conflicting callers and a capacity-two saturation case. It proves single-process multithreaded coordination, not multi-process shared-memory synchronization or distributed request arbitration.

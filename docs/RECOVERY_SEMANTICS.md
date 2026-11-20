# Durable request recovery semantics

The vehicle-service runtime uses an append-only journal for methods marked `state_changing`. The goal is to prevent blind duplicate execution after a process restart while preserving an honest `COMPLETION_UNKNOWN` state when the runtime cannot prove the outcome.

## Write ordering

```text
validate service / method / permission / deadline
  -> append and flush BEGIN(request_id, semantic_fingerprint)
  -> dispatch handler
  -> append and flush COMPLETE(request_id, fingerprint, typed_response)
  -> return response
```

On startup, a valid `BEGIN + COMPLETE` pair restores the original typed response. A `BEGIN` without `COMPLETE` restores a terminal `COMPLETION_UNKNOWN` response. The same Request ID is therefore not dispatched again after an uncertain crash window.

Each line is hex-encoded where needed and protected by CRC-32. A malformed schema, invalid hex field, unmatched completion or checksum mismatch rejects startup instead of silently discarding evidence.

## Failure interpretation

| Last durable record | Meaning after restart | Runtime action |
|---|---|---|
| none | handler was not admitted | request may be submitted normally |
| `BEGIN` only | handler may have started or completed | return `COMPLETION_UNKNOWN`; do not re-execute |
| `BEGIN + COMPLETE` | terminal response is known | replay the original typed response |
| corrupted journal | recovery truth cannot be established | fail startup closed |

## Current boundaries

- one runtime process is the journal writer;
- calls are still sequential; concurrent in-flight duplicate coordination is not implemented;
- `flush()` supports the measured process-restart experiment but is not claimed as power-loss durability or storage-barrier proof;
- no journal compaction, retention policy or multi-process locking exists yet;
- persistent response encoding currently covers the state-changing response types used by the project;
- no transactional coupling exists with a physical actuator or external ECU.

The evidence therefore supports process restart and simulated handler crash claims, not distributed exactly-once execution or crash consistency under sudden power loss.

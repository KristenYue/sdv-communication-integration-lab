# Engineering log

## 2026-11-20 - Cross-process shutdown race

### Observation

The first 220-scenario run completed every frame exchange, but the controller reported that the C++ gateway exited with code 1.

### Cause

After reading the gateway's `QUIT` acknowledgement, the Python controller immediately entered its cleanup block. The child process had emitted the acknowledgement but had not yet completed normal process teardown, so `poll()` still returned `None` and the controller terminated it.

### Fix

The controller now waits up to five seconds for a normal exit after the acknowledged `QUIT`. Forced termination remains only as cleanup for a hung or failed child.

### Regression evidence

- The same frozen corpus hash is retained.
- 220/220 scenarios and 300/300 frame submissions complete.
- The child exits normally with code 0.
- This was a controller lifecycle defect, not a vehicle-frame validation defect.

The incident is kept because process lifecycle and shutdown ordering are part of system integration work; a successful business response does not prove that the worker lifecycle is correct.

## 2026-11-20 - False failures from cross-language float equality

### Observation

The first 200-system-scenario run reported 160 passed and 40 failed. Every gateway result code and freshness assertion was correct, but selected vehicle-speed snapshots failed the state-preservation comparison.

### Cause

The C++ process serialized a decoded speed as a short decimal such as `40.05`. Python independently calculated the expected value from the raw integer and scale, producing an in-memory representation such as `40.050000000000004`. The verifier incorrectly required exact binary floating-point equality across language and serialization boundaries.

### Fix

Floating-point snapshot fields now use an explicit absolute tolerance of `1e-9`. Integer, Boolean, list and string fields still require exact equality. The frozen inputs and their SHA-256 were not changed.

### Lesson

An integration oracle is part of the system under test. Cross-language numeric contracts must define comparison semantics instead of relying on host-language equality.

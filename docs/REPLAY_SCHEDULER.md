# Leased replay-worker scheduler

## Local metadata model

The scheduler uses SQLite WAL mode as a reproducible local metadata backend. Each replay job stores status, lease owner, lease expiry, monotonically increasing fencing token, attempt count, result SHA-256, expected-contract outcome and update time.

Lease acquisition runs inside `BEGIN IMMEDIATE` and may select a pending task or an expired lease. Every successful acquisition increments both the attempt and fencing token. A heartbeat extends only the currently leased row whose owner and token still match and whose lease has not expired.

## Commit semantics

```text
current owner + current token + active lease
  -> first result                 = COMMITTED
  -> same result submitted again = IDEMPOTENT_REPLAY
  -> different result            = RESULT_CONFLICT

old owner or old fencing token    = FENCED
expired current lease             = LEASE_EXPIRED
```

A completed task retains the winning owner and fencing token. Consequently, a stale worker cannot disguise its late commit as an idempotent replay of the winner's result.

## Executable evidence

Four Python worker threads each own an independent C++ gateway process. They acquire tasks from the shared SQLite database, heartbeat before and after actual gateway execution, and commit the canonical output digest. The measured Windows run completed all 300 tasks with 300 expected-contract matches, no remaining leases and an aggregate output SHA-256 identical to the earlier single-process replay pipeline.

An additional 60 deterministic lease-control experiments verify heartbeat protection, lease-expiry takeover, monotonic fencing, stale-worker rejection, idempotent winning-worker resubmission and conflicting-result rejection.

## Boundaries

- SQLite is a local evidence backend, not PostgreSQL or a high-availability control plane;
- workers are threads in one Python coordinator, while each gateway is a separate C++ process;
- task inputs and result metadata remain local JSONL/SQLite files;
- no object storage, MCAP/Parquet payload store, multi-host clock model or network partition exists;
- the control experiments use synthetic timestamps to make lease transitions deterministic.

## PostgreSQL migration path

`PostgresReplayJobStore` preserves the same lease/heartbeat/fencing/commit contract while replacing the SQLite-wide write transaction with dataset-scoped row locks and `FOR UPDATE SKIP LOCKED`. All lease decisions use PostgreSQL server time. A dedicated CI job provisions PostgreSQL 16 and is designed to execute 8 independent database connections and C++ gateway processes; see [POSTGRES_REPLAY.md](POSTGRES_REPLAY.md).

The PostgreSQL job has not been executed on this local Windows environment because no PostgreSQL or container runtime is installed. Until the remote CI report exists, this is implemented integration coverage rather than measured PostgreSQL evidence.

# Versioned vehicle replay pipeline

## Purpose

The replay pipeline turns the frozen four-node vehicle corpus into versioned operational tasks. It is intended as the first data-loop primitive for diagnostic reproduction, regression comparison and later batch-worker scheduling. It is not presented as a test-case management product.

## Task dataset

`data/replay/tasks_v1.jsonl` contains 300 unique task IDs derived from 200 unique source scenarios:

- generation 1: all 200 source scenarios;
- generation 2: the first 100 source scenarios repeated as independently addressable replay tasks;
- each task embeds the complete scenario, source-scenario ID, source-dataset SHA-256 and generation;
- the manifest freezes task count, source count, generation distribution and dataset SHA-256.

The repeated generation is explicit. The dataset must not be described as 300 unique vehicle scenarios.

## Execution and comparison

```text
versioned task dataset
  -> baseline C++ gateway process
  -> ordered frame-result events + terminal snapshot
  -> canonical per-task SHA-256

same task dataset
  -> fresh replay C++ gateway process
  -> ordered frame-result events + terminal snapshot
  -> canonical per-task SHA-256

compare expected contract + baseline digest + replay digest
```

The canonical output includes ordered result code, acceptance flag, CAN ID, Trace ID and the complete JSON snapshot. A deterministic match requires an exact SHA-256 match between baseline and replay. Expected-code, freshness and terminal-state checks are performed independently in both phases.

## Current evidence boundary

The Windows report contains 300 tasks, 200 unique source scenarios, 2,600 total frame submissions, 600/600 expected-contract matches and 300/300 deterministic digest matches. Both aggregate output hashes are identical.

The current format is JSONL plus JSON reports. MCAP/Parquet, object storage, PostgreSQL metadata, leases, heartbeats, fencing, multi-worker scheduling and long-term retention are not implemented yet. The two phases each use one fresh gateway process; this is not a distributed replay cluster.

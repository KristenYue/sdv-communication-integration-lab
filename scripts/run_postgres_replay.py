from __future__ import annotations

import argparse
import hashlib
import json
import platform
import time
from collections import Counter
from pathlib import Path

from sdv_lab.postgres_replay_scheduler import (
    PostgresReplayJobStore,
    apply_migration,
)
from sdv_lab.replay_tasks import build_replay_tasks, replay_dataset_sha256
from sdv_lab.scheduled_replay import execute_scheduled_replay_with_store
from sdv_lab.system_scenarios import build_system_scenarios


def _digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def execute_postgres_lease_controls(dsn: str, count: int = 60) -> dict:
    dataset_sha = _digest(f"postgres-lease-controls-v1:{count}")
    store = PostgresReplayJobStore(dsn, dataset_sha, "postgres-lease-controls-v1", count)
    task_ids = [f"PG-LEASE-CONTROL-{index + 1:04d}" for index in range(count)]
    store.seed(task_ids)
    results: Counter[str] = Counter()
    first_leases = []
    for task_id in task_ids:
        lease = store.claim_task(task_id, "worker-a", 0, 5_000)
        if lease is None:
            raise RuntimeError(f"worker-a could not claim {task_id}")
        first_leases.append(lease)
    for lease in first_leases:
        if store.heartbeat(lease, 0, 5_000):
            results["heartbeats_accepted"] += 1
    for task_id in task_ids:
        if store.claim_task(task_id, "worker-b", 0, 10_000) is None:
            results["premature_takeovers_blocked"] += 1

    time.sleep(5.2)
    second_leases = []
    for task_id in task_ids:
        lease = store.claim_task(task_id, "worker-b", 0, 10_000)
        if lease is None:
            raise RuntimeError(f"worker-b could not take over {task_id}")
        second_leases.append(lease)
    for index, (first, second) in enumerate(zip(first_leases, second_leases, strict=True)):
        if second.fencing_token <= first.fencing_token:
            raise RuntimeError(f"fencing token did not advance for {second.task_id}")
        digest = _digest(f"postgres-result-{index:04d}")
        results[store.commit(second, digest, True, 0)] += 1
        results[store.commit(first, digest, True, 0)] += 1
        results[store.commit(second, digest, True, 0)] += 1
        results[store.commit(second, _digest(digest + "-conflict"), True, 0)] += 1
    return {
        "experiments": count,
        "heartbeats_accepted": results["heartbeats_accepted"],
        "premature_takeovers_blocked": results["premature_takeovers_blocked"],
        "takeover_commits": results["COMMITTED"],
        "stale_commits_fenced": results["FENCED"],
        "idempotent_commit_replays": results["IDEMPOTENT_REPLAY"],
        "result_conflicts_rejected": results["RESULT_CONFLICT"],
        "server_clock_wait_seconds": 5.2,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dsn", required=True)
    parser.add_argument("--gateway-bin", required=True)
    parser.add_argument("--migration", default="migrations/0001_postgresql_replay_metadata.sql")
    parser.add_argument("--output", default="artifacts/postgres-replay-v1.json")
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    apply_migration(args.dsn, args.migration)
    tasks = build_replay_tasks(build_system_scenarios(), 300)
    dataset_sha = replay_dataset_sha256(tasks)
    store = PostgresReplayJobStore(args.dsn, dataset_sha, "sdv-replay-storage-v1", 200)
    batch = execute_scheduled_replay_with_store(args.gateway_bin, tasks, store, args.workers)

    reopened = PostgresReplayJobStore(args.dsn, dataset_sha, "sdv-replay-storage-v1", 200)
    reopened_results = reopened.completed_results()
    metadata_reopened = len(reopened_results) == 300 and reopened.counts() == {
        "pending": 0,
        "leased": 0,
        "completed": 300,
    }
    controls = execute_postgres_lease_controls(args.dsn, 60)
    expected_controls = {
        "experiments": 60,
        "heartbeats_accepted": 60,
        "premature_takeovers_blocked": 60,
        "takeover_commits": 60,
        "stale_commits_fenced": 60,
        "idempotent_commit_replays": 60,
        "result_conflicts_rejected": 60,
        "server_clock_wait_seconds": 5.2,
    }
    if not (
        batch["completed"] == 300
        and batch["expected_matches"] == 300
        and batch["commit_statuses"] == {"COMMITTED": 300}
        and batch["final_job_counts"] == {"pending": 0, "leased": 0, "completed": 300}
        and metadata_reopened
        and controls == expected_controls
    ):
        raise RuntimeError(
            f"PostgreSQL replay acceptance failed: {batch=} {metadata_reopened=} {controls=}"
        )
    report = {
        "schema_version": "sdv-postgres-replay-report-v1",
        "data_boundary": (
            "postgresql_service_container_with_concurrent_connections_and_cpp_gateway_processes;"
            "not_multi_host_or_database_restart_evidence"
        ),
        "platform": platform.platform(),
        "postgresql": reopened.server_info(),
        "batch": batch,
        "metadata_reopened": metadata_reopened,
        "lease_controls": controls,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

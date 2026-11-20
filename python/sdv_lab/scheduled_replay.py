"""Multi-worker replay execution over leased SQLite job metadata."""

from __future__ import annotations

import json
import subprocess
import threading
import time
from collections import Counter
from pathlib import Path

from .replay_runner import _canonical_sha256
from .replay_scheduler import ReplayJobStore
from .replay_tasks import ReplayTask
from .system_runner import FRESHNESS_FIELDS, _value_matches


def _now_ms() -> int:
    return time.monotonic_ns() // 1_000_000


class GatewayReplayWorker:
    def __init__(self, executable: str | Path):
        self.process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )

    def exchange(self, line: str) -> dict:
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("gateway process pipes are unavailable")
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()
        raw = self.process.stdout.readline()
        if not raw:
            raise RuntimeError("gateway process closed output unexpectedly")
        return json.loads(raw)

    def execute(self, task: ReplayTask) -> tuple[str, bool]:
        if self.exchange("RESET") != {"kind": "reset", "ok": True}:
            raise RuntimeError(f"gateway reset failed for {task.task_id}")
        events = []
        for frame in task.scenario.frames:
            result = self.exchange(
                f"FRAME\t{frame.frame_id}\t{frame.dlc}\t{frame.data_hex}\t"
                f"{frame.bus_time_ms}\t{frame.trace_id}"
            )
            events.append(
                {
                    "code": result.get("code"),
                    "accepted": result.get("accepted"),
                    "frame_id": result.get("frame_id"),
                    "trace_id": result.get("trace_id"),
                }
            )
        snapshot = self.exchange(f"SNAPSHOT\t{task.scenario.snapshot_now_ms}\t100")
        actual_codes = [str(event["code"]) for event in events]
        expected_match = actual_codes == list(task.scenario.expected_codes)
        expected_match = expected_match and all(
            snapshot.get(field) is False for field in FRESHNESS_FIELDS
        )
        expected_match = expected_match and all(
            _value_matches(snapshot.get(field), expected)
            for field, expected in task.scenario.expected_snapshot.items()
        )
        return _canonical_sha256({"events": events, "snapshot": snapshot}), expected_match

    def close(self) -> None:
        try:
            if self.process.poll() is None:
                self.exchange("QUIT")
                self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.terminate()
            _, stderr = self.process.communicate(timeout=5)
        if self.process.returncode != 0:
            raise RuntimeError(f"gateway worker exited with {self.process.returncode}: {stderr}")


def execute_scheduled_replay(
    executable: str | Path,
    tasks: list[ReplayTask],
    database: str | Path,
    worker_count: int = 4,
) -> dict:
    store = ReplayJobStore(database)
    store.seed([task.task_id for task in tasks], _now_ms())
    tasks_by_id = {task.task_id: task for task in tasks}
    statistics: Counter[str] = Counter()
    worker_commits: Counter[str] = Counter()
    failures: list[str] = []
    lock = threading.Lock()
    started = time.perf_counter()

    def worker_main(worker_index: int) -> None:
        worker_id = f"replay-worker-{worker_index}"
        gateway = GatewayReplayWorker(executable)
        try:
            while True:
                lease = store.acquire_next(worker_id, _now_ms(), 5_000)
                if lease is None:
                    counts = store.counts()
                    if counts["completed"] == len(tasks):
                        return
                    time.sleep(0.005)
                    continue
                if not store.heartbeat(lease, _now_ms(), 5_000):
                    raise RuntimeError(f"initial heartbeat rejected for {lease.task_id}")
                result_sha, expected_match = gateway.execute(tasks_by_id[lease.task_id])
                if not store.heartbeat(lease, _now_ms(), 5_000):
                    raise RuntimeError(f"completion heartbeat rejected for {lease.task_id}")
                status = store.commit(lease, result_sha, expected_match, _now_ms())
                with lock:
                    statistics[status] += 1
                    worker_commits[worker_id] += status == "COMMITTED"
                if status != "COMMITTED":
                    raise RuntimeError(f"unexpected commit status {status} for {lease.task_id}")
        except Exception as error:  # noqa: BLE001 - thread boundary forwards failures
            with lock:
                failures.append(f"{worker_id}: {error}")
        finally:
            gateway.close()

    threads = [threading.Thread(target=worker_main, args=(index,)) for index in range(worker_count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=30)
    if any(thread.is_alive() for thread in threads):
        raise RuntimeError("scheduled replay workers did not terminate")
    if failures:
        raise RuntimeError("; ".join(failures))

    results = store.completed_results()
    aggregate_sha = _canonical_sha256([(task_id, result_sha) for task_id, result_sha, _ in results])
    return {
        "task_count": len(tasks),
        "worker_count": worker_count,
        "gateway_processes": worker_count,
        "completed": len(results),
        "expected_matches": sum(expected for _, _, expected in results),
        "commit_statuses": dict(sorted(statistics.items())),
        "worker_commits": dict(sorted(worker_commits.items())),
        "aggregate_output_sha256": aggregate_sha,
        "elapsed_seconds": time.perf_counter() - started,
        "final_job_counts": store.counts(),
    }


def execute_lease_control_experiments(database: str | Path, count: int = 60) -> dict:
    store = ReplayJobStore(database)
    task_ids = [f"LEASE-CONTROL-{index + 1:04d}" for index in range(count)]
    store.seed(task_ids)
    results: Counter[str] = Counter()
    for index, task_id in enumerate(task_ids):
        first = store.claim_task(task_id, "worker-a", 0, 10)
        if first is None:
            raise RuntimeError(f"worker-a could not claim {task_id}")
        if store.heartbeat(first, 5, 10):
            results["heartbeats_accepted"] += 1
        if store.claim_task(task_id, "worker-b", 11, 10) is None:
            results["premature_takeovers_blocked"] += 1
        second = store.claim_task(task_id, "worker-b", 16, 10)
        if second is None or second.fencing_token <= first.fencing_token:
            raise RuntimeError(f"worker-b could not take over {task_id}")
        digest = f"result-{index:04d}"
        results[store.commit(second, digest, True, 17)] += 1
        results[store.commit(first, digest, True, 17)] += 1
        results[store.commit(second, digest, True, 17)] += 1
        results[store.commit(second, digest + "-conflict", True, 17)] += 1
    return {
        "experiments": count,
        "heartbeats_accepted": results["heartbeats_accepted"],
        "premature_takeovers_blocked": results["premature_takeovers_blocked"],
        "takeover_commits": results["COMMITTED"],
        "stale_commits_fenced": results["FENCED"],
        "idempotent_commit_replays": results["IDEMPOTENT_REPLAY"],
        "result_conflicts_rejected": results["RESULT_CONFLICT"],
    }

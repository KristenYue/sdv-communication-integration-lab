"""Record and replay frozen vehicle tasks in independent C++ gateway processes."""

from __future__ import annotations

import hashlib
import json
import platform
import subprocess
import time
from pathlib import Path

from .replay_tasks import ReplayTask, replay_dataset_sha256
from .system_runner import FRESHNESS_FIELDS, _value_matches


def _canonical_sha256(value: object) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(encoded).hexdigest()


def _exchange(process: subprocess.Popen[str], line: str) -> dict:
    if process.stdin is None or process.stdout is None:
        raise RuntimeError("gateway process pipes are unavailable")
    process.stdin.write(line + "\n")
    process.stdin.flush()
    raw = process.stdout.readline()
    if not raw:
        raise RuntimeError("gateway process closed its output unexpectedly")
    return json.loads(raw)


def _run_phase(executable: str | Path, tasks: list[ReplayTask]) -> tuple[list[dict], float]:
    process = subprocess.Popen(
        [str(executable)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    records: list[dict] = []
    started = time.perf_counter()
    try:
        for task in tasks:
            if _exchange(process, "RESET") != {"kind": "reset", "ok": True}:
                raise RuntimeError(f"gateway reset failed for {task.task_id}")
            events = []
            for frame in task.scenario.frames:
                result = _exchange(
                    process,
                    f"FRAME\t{frame.frame_id}\t{frame.dlc}\t{frame.data_hex}\t"
                    f"{frame.bus_time_ms}\t{frame.trace_id}",
                )
                events.append(
                    {
                        "code": result.get("code"),
                        "accepted": result.get("accepted"),
                        "frame_id": result.get("frame_id"),
                        "trace_id": result.get("trace_id"),
                    }
                )
            snapshot = _exchange(
                process, f"SNAPSHOT\t{task.scenario.snapshot_now_ms}\t100"
            )
            expected_codes = list(task.scenario.expected_codes)
            actual_codes = [str(event["code"]) for event in events]
            expected_match = actual_codes == expected_codes and all(
                snapshot.get(field) is False for field in FRESHNESS_FIELDS
            )
            expected_match = expected_match and all(
                _value_matches(snapshot.get(field), expected)
                for field, expected in task.scenario.expected_snapshot.items()
            )
            output = {"events": events, "snapshot": snapshot}
            records.append(
                {
                    "task_id": task.task_id,
                    "source_scenario_id": task.source_scenario_id,
                    "generation": task.generation,
                    "expected_match": expected_match,
                    "output_sha256": _canonical_sha256(output),
                }
            )
        if _exchange(process, "QUIT") != {"kind": "quit", "ok": True}:
            raise RuntimeError("gateway quit acknowledgement failed")
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
        _, stderr = process.communicate(timeout=5)
    if process.returncode != 0:
        raise RuntimeError(f"gateway exited with {process.returncode}: {stderr}")
    return records, time.perf_counter() - started


def execute_replay_pipeline(executable: str | Path, tasks: list[ReplayTask]) -> dict:
    baseline, baseline_seconds = _run_phase(executable, tasks)
    replay, replay_seconds = _run_phase(executable, tasks)
    replay_by_id = {record["task_id"]: record for record in replay}
    divergences = []
    deterministic_matches = 0
    for original in baseline:
        repeated = replay_by_id[original["task_id"]]
        if original["output_sha256"] == repeated["output_sha256"]:
            deterministic_matches += 1
        else:
            divergences.append(
                {
                    "task_id": original["task_id"],
                    "baseline_sha256": original["output_sha256"],
                    "replay_sha256": repeated["output_sha256"],
                }
            )
    expected_matches = sum(record["expected_match"] for record in baseline) + sum(
        record["expected_match"] for record in replay
    )
    frame_count_per_phase = sum(len(task.scenario.frames) for task in tasks)
    baseline_digest = _canonical_sha256(
        [(record["task_id"], record["output_sha256"]) for record in baseline]
    )
    replay_digest = _canonical_sha256(
        [(record["task_id"], record["output_sha256"]) for record in replay]
    )
    return {
        "schema_version": "sdv-replay-report-v1",
        "data_boundary": "versioned_synthetic_vehicle_tasks_over_two_fresh_cpp_processes",
        "platform": platform.platform(),
        "gateway_executable": Path(executable).name,
        "task_dataset_sha256": replay_dataset_sha256(tasks),
        "task_count": len(tasks),
        "unique_source_scenarios": len({task.source_scenario_id for task in tasks}),
        "baseline_processes": 1,
        "replay_processes": 1,
        "frame_submissions_per_phase": frame_count_per_phase,
        "total_frame_submissions": frame_count_per_phase * 2,
        "expected_matches": expected_matches,
        "expected_match_total": len(tasks) * 2,
        "deterministic_matches": deterministic_matches,
        "deterministic_mismatches": len(divergences),
        "baseline_output_sha256": baseline_digest,
        "replay_output_sha256": replay_digest,
        "baseline_elapsed_seconds": baseline_seconds,
        "replay_elapsed_seconds": replay_seconds,
        "divergences": divergences,
    }

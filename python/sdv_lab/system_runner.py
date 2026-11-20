"""Execute the four-node frozen system corpus against the C++ gateway process."""

from __future__ import annotations

import json
import platform
import subprocess
import time
from collections import Counter
from pathlib import Path

from .system_scenarios import SystemScenario, system_dataset_sha256

FRESHNESS_FIELDS = (
    "vehicle_status_stale",
    "tire_pressure_stale",
    "body_status_stale",
    "tbox_health_stale",
)


def _exchange(process: subprocess.Popen, line: str) -> tuple[dict, float]:
    if process.stdin is None or process.stdout is None:
        raise RuntimeError("gateway pipes are unavailable")
    started = time.perf_counter_ns()
    process.stdin.write(line + "\n")
    process.stdin.flush()
    response = json.loads(process.stdout.readline())
    return response, (time.perf_counter_ns() - started) / 1_000_000


def _percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    return ordered[min(len(ordered) - 1, round((len(ordered) - 1) * fraction))]


def _value_matches(actual, expected) -> bool:
    if isinstance(expected, float):
        return isinstance(actual, (int, float)) and abs(float(actual) - expected) <= 1e-9
    return actual == expected


def execute_system_corpus(executable: str | Path, scenarios: list[SystemScenario]) -> dict:
    process = subprocess.Popen(
        [str(executable)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    failures: list[dict] = []
    latencies: list[float] = []
    codes: Counter[str] = Counter()
    snapshots_preserved = 0
    frame_count = 0
    started = time.perf_counter()
    try:
        for scenario in scenarios:
            reset, _ = _exchange(process, "RESET")
            if reset != {"kind": "reset", "ok": True}:
                raise RuntimeError(f"gateway reset failed: {reset}")
            actual_codes: list[str] = []
            for frame in scenario.frames:
                line = (
                    f"FRAME\t{frame.frame_id}\t{frame.dlc}\t{frame.data_hex}\t"
                    f"{frame.bus_time_ms}\t{frame.trace_id}"
                )
                result, latency = _exchange(process, line)
                latencies.append(latency)
                frame_count += 1
                code = str(result.get("code"))
                codes[code] += 1
                actual_codes.append(code)
            snapshot, latency = _exchange(
                process, f"SNAPSHOT\t{scenario.snapshot_now_ms}\t100"
            )
            latencies.append(latency)
            codes_match = tuple(actual_codes) == scenario.expected_codes
            fresh = all(snapshot.get(field) is False for field in FRESHNESS_FIELDS)
            state_preserved = all(
                _value_matches(snapshot.get(field), expected)
                for field, expected in scenario.expected_snapshot.items()
            )
            if fresh and state_preserved:
                snapshots_preserved += 1
            if not (codes_match and fresh and state_preserved):
                failures.append(
                    {
                        "scenario_id": scenario.scenario_id,
                        "category": scenario.category,
                        "expected_codes": list(scenario.expected_codes),
                        "actual_codes": actual_codes,
                        "fresh": fresh,
                        "state_preserved": state_preserved,
                        "snapshot": snapshot,
                    }
                )
        quit_response, _ = _exchange(process, "QUIT")
        if quit_response != {"kind": "quit", "ok": True}:
            raise RuntimeError(f"unexpected quit response: {quit_response}")
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
        _, stderr = process.communicate(timeout=5)
    if process.returncode != 0:
        raise RuntimeError(f"gateway exited with {process.returncode}: {stderr}")

    fault_count = sum(item.fault_injected for item in scenarios)
    return {
        "schema_version": "sdv-system-scenario-report-v1",
        "data_boundary": "deterministic_four_node_synthetic_system_scenarios",
        "platform": platform.platform(),
        "gateway_executable": Path(executable).name,
        "dataset_sha256": system_dataset_sha256(scenarios),
        "scenario_count": len(scenarios),
        "healthy_scenarios": len(scenarios) - fault_count,
        "fault_injection_scenarios": fault_count,
        "frame_count": frame_count,
        "snapshots_preserved": snapshots_preserved,
        "passed": len(scenarios) - len(failures),
        "failed": len(failures),
        "gateway_codes": dict(sorted(codes.items())),
        "elapsed_seconds": time.perf_counter() - started,
        "control_path_latency_ms": {
            "p50": _percentile(latencies, 0.50),
            "p95": _percentile(latencies, 0.95),
            "p99": _percentile(latencies, 0.99),
            "max": max(latencies, default=0.0),
            "scope": "local process pipe plus C++ gateway; not CAN latency",
        },
        "failures": failures,
    }

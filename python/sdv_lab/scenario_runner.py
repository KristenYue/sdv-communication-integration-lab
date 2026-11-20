"""Cross-process executor for the frozen SDV gateway scenarios."""

from __future__ import annotations

import json
import math
import platform
import subprocess
import time
from collections import Counter
from pathlib import Path

from .scenarios import FrozenScenario, dataset_sha256


def _percentile(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def execute_corpus(executable: str | Path, scenarios: list[FrozenScenario]) -> dict:
    process = subprocess.Popen(
        [str(executable)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    if process.stdin is None or process.stdout is None:
        raise RuntimeError("failed to open gateway process pipes")

    latencies_ms: list[float] = []
    observed_codes: Counter[str] = Counter()
    failures: list[dict] = []
    total_frames = 0
    started = time.perf_counter()
    try:
        for scenario in scenarios:
            process.stdin.write("RESET\n")
            process.stdin.flush()
            reset = json.loads(process.stdout.readline())
            if reset != {"kind": "reset", "ok": True}:
                raise RuntimeError(f"gateway reset failed: {reset}")

            actual: list[str] = []
            for frame in scenario.frames:
                before = time.perf_counter_ns()
                process.stdin.write(frame.cli_line() + "\n")
                process.stdin.flush()
                result = json.loads(process.stdout.readline())
                latencies_ms.append((time.perf_counter_ns() - before) / 1_000_000)
                total_frames += 1
                code = str(result.get("code"))
                actual.append(code)
                observed_codes[code] += 1
            if tuple(actual) != scenario.expected_codes:
                failures.append(
                    {
                        "scenario_id": scenario.scenario_id,
                        "category": scenario.category,
                        "expected": list(scenario.expected_codes),
                        "actual": actual,
                    }
                )
        process.stdin.write("QUIT\n")
        process.stdin.flush()
        json.loads(process.stdout.readline())
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
        _, stderr = process.communicate(timeout=5)
    if process.returncode not in (0, None):
        raise RuntimeError(f"gateway exited with {process.returncode}: {stderr}")

    elapsed_seconds = time.perf_counter() - started
    return {
        "schema_version": "sdv-scenario-report-v1",
        "data_boundary": "deterministic_synthetic_frames_over_local_process_pipe",
        "gateway_executable": Path(executable).name,
        "platform": platform.platform(),
        "python": platform.python_version(),
        "dataset_sha256": dataset_sha256(scenarios),
        "scenario_count": len(scenarios),
        "frame_count": total_frames,
        "passed": len(scenarios) - len(failures),
        "failed": len(failures),
        "elapsed_seconds": elapsed_seconds,
        "control_path_latency_ms": {
            "p50": _percentile(latencies_ms, 0.50),
            "p95": _percentile(latencies_ms, 0.95),
            "p99": _percentile(latencies_ms, 0.99),
            "max": max(latencies_ms, default=0.0),
            "scope": "Python pipe round-trip plus C++ validation; not CAN bus latency",
        },
        "observed_codes": dict(sorted(observed_codes.items())),
        "failures": failures,
    }

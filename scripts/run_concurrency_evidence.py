from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", default="artifacts/runtime-concurrency-v1.json")
    args = parser.parse_args()

    completed = subprocess.run(
        [args.executable], check=False, capture_output=True, text=True, timeout=15
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"concurrency evidence failed: {completed.stdout.strip()} {completed.stderr.strip()}"
        )
    result = json.loads(completed.stdout)
    if not (
        result["identical_threads"] == 32
        and result["identical_successes"] == 32
        and result["identical_handler_executions"] == 1
        and result["idempotent_replay_events"] == 31
        and result["in_flight_wait_events"] >= 31
        and result["conflict_threads"] == 16
        and result["conflicts_rejected"] == 16
        and result["conflict_leader_accepted"] is True
        and result["capacity_limit"] == 2
        and result["capacity_started"] == 2
        and result["no_deadline_result"] == "QUEUE_FULL"
        and result["queued_result"] == "QUEUE_TIMEOUT"
    ):
        raise RuntimeError(f"unexpected concurrency result: {result}")
    report = {
        "schema_version": "sdv-runtime-concurrency-v1",
        "data_boundary": "single_process_multithreaded_runtime_synthetic_handlers",
        "platform": platform.platform(),
        **result,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

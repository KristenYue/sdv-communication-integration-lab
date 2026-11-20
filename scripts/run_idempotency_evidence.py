from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", default="artifacts/runtime-idempotency-v1.json")
    args = parser.parse_args()

    completed = subprocess.run(
        [args.executable], check=False, capture_output=True, text=True, timeout=10
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"idempotency evidence failed: {completed.stdout.strip()} {completed.stderr.strip()}"
        )
    result = json.loads(completed.stdout)
    expected = {
        "groups": 60,
        "total_invocations": 180,
        "first_successes": 60,
        "replay_successes": 60,
        "conflicts_rejected": 60,
        "handler_executions": 60,
        "duplicate_final_actions": 0,
        "idempotent_replay_events": 60,
    }
    if result != expected:
        raise RuntimeError(f"unexpected idempotency result: {result}")
    report = {
        "schema_version": "sdv-runtime-idempotency-v1",
        "data_boundary": "single_process_sequential_in_memory_idempotency_cache",
        "platform": platform.platform(),
        **result,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

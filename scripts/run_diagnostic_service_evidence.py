from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", default="artifacts/diagnostic-service-evidence-v1.json")
    args = parser.parse_args()

    completed = subprocess.run(
        [args.executable], check=False, capture_output=True, text=True, timeout=30
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "diagnostic service evidence failed: "
            f"{completed.stdout.strip()} {completed.stderr.strip()}"
        )
    result = json.loads(completed.stdout)
    expected = {
        "discovered_methods": 5,
        "permission_rejections": 50,
        "dtcs_read": 10,
        "default_clear_code": "SESSION_DENIED",
        "session_accepted": True,
        "seed_returned": True,
        "key_accepted": True,
        "clear_accepted": True,
        "clear_replay_accepted": True,
        "idempotent_replays": 1,
        "active_after_clear": 0,
    }
    if any(result.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"unexpected diagnostic service evidence: {result}")
    report = {
        "schema_version": "sdv-diagnostic-service-evidence-v1",
        "data_boundary": (
            "in_process_typed_vehicle_service_runtime_and_synthetic_dtcs;"
            "not_remote_uds_doip_or_physical_ecu_evidence"
        ),
        "platform": platform.platform(),
        **result,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

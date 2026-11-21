from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--journal", default="artifacts/diagnostic-evidence.journal")
    parser.add_argument("--output", default="artifacts/diagnostic-evidence-v1.json")
    args = parser.parse_args()

    journal = Path(args.journal)
    journal.parent.mkdir(parents=True, exist_ok=True)
    journal.unlink(missing_ok=True)
    completed = subprocess.run(
        [args.executable, str(journal)], check=False, capture_output=True, text=True, timeout=30
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"diagnostic evidence failed: {completed.stdout.strip()} {completed.stderr.strip()}"
        )
    result = json.loads(completed.stdout)
    expected = {
        "registered_dtcs": 20,
        "failure_reports": 40,
        "same_cycle_replays": 20,
        "confirmed_before_restart": 20,
        "freeze_frames_before_restart": 20,
        "recovered_confirmed": 20,
        "unauthorized_clears": 30,
        "security_lockouts": 20,
        "authorized_clear": True,
        "active_after_clear_restart": 0,
        "corruption_rejected": True,
    }
    if any(result.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"unexpected diagnostic evidence: {result}")
    report = {
        "schema_version": "sdv-diagnostic-evidence-v1",
        "data_boundary": (
            "bounded_uds_style_state_machine_and_synthetic_dtcs;"
            "not_iso_14229_conformance_or_physical_ecu_diagnostics"
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

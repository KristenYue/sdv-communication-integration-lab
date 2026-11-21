from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument(
        "--output", default="artifacts/gateway-diagnostic-bridge-evidence.json"
    )
    args = parser.parse_args()

    completed = subprocess.run(
        [args.executable], check=False, capture_output=True, text=True, timeout=30
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "gateway diagnostic bridge evidence failed: "
            f"{completed.stdout.strip()} {completed.stderr.strip()}"
        )
    result = json.loads(completed.stdout)
    expected = {
        "registered_monitors": 8,
        "fault_cycles": 2,
        "active_dtcs": 8,
        "confirmed_dtcs": 8,
        "freeze_frames": 8,
        "occurrence_count_two": 8,
        "distinct_source_traces": 8,
        "typed_service_dtcs": 8,
        "wire_adapter_dtcs": 8,
        "wire_decoded": True,
        "gateway_trace_events": 16,
        "diagnostic_trace_events": 18,
        "runtime_trace_events": 4,
    }
    if any(result.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"unexpected gateway diagnostic bridge evidence: {result}")
    report = {
        "schema_version": "sdv-gateway-diagnostic-bridge-evidence-v1",
        "data_boundary": (
            "synthetic_can_frames_to_gateway_monitors_to_dtc_to_typed_service_and_"
            "bounded_someip_adapter;not_physical_can_ecu_or_production_monitoring"
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

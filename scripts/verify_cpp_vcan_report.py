from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    if report.get("socketcan_used") is not True:
        raise AssertionError("report does not prove SocketCAN use")
    if report.get("transport_boundary") != "linux_virtual_can_not_physical_vehicle_bus":
        raise AssertionError("transport boundary is missing or incorrect")
    if report.get("diagnostic_bridge_enabled"):
        expected_codes = [
            "APPLIED",
            "SIGNAL_OUT_OF_RANGE",
            "INVALID_DLC",
            "UNKNOWN_CAN_ID",
        ] * 2
        if [item["code"] for item in report["results"]] != expected_codes:
            raise AssertionError("unexpected diagnostic SocketCAN gateway results")
        expected_diagnostic = {
            "cycles": 2,
            "active_dtcs": 6,
            "confirmed_dtcs": 6,
            "freeze_frames": 6,
            "occurrence_count_two": 6,
            "wire_dtcs": 6,
            "trace_events": 18,
        }
        if report.get("diagnostic") != expected_diagnostic:
            raise AssertionError(f"unexpected SocketCAN diagnostic result: {report}")
    else:
        if [item["frame_id"] for item in report["results"]] != [256, 257]:
            raise AssertionError("unexpected SocketCAN frame sequence")
        if [item["code"] for item in report["results"]] != ["APPLIED", "APPLIED"]:
            raise AssertionError("C++ gateway did not apply both virtual-ECU frames")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

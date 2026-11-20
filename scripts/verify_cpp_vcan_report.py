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
    if [item["frame_id"] for item in report["results"]] != [256, 257]:
        raise AssertionError("unexpected SocketCAN frame sequence")
    if [item["code"] for item in report["results"]] != ["APPLIED", "APPLIED"]:
        raise AssertionError("C++ gateway did not apply both virtual-ECU frames")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

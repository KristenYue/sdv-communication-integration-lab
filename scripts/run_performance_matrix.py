from __future__ import annotations

import argparse
import json
import platform
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", default="artifacts/runtime-performance-matrix-v1.json")
    args = parser.parse_args()

    completed = subprocess.run(
        [args.executable], check=False, capture_output=True, text=True, timeout=30
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"performance matrix failed: {completed.stdout[-1000:]} {completed.stderr[-1000:]}"
        )
    report = json.loads(completed.stdout)
    cells = report["cells"]
    dimensions = {
        "frequencies_hz": sorted({cell["frequency_hz"] for cell in cells}),
        "payload_bytes": sorted({cell["payload_bytes"] for cell in cells}),
        "node_counts": sorted({cell["nodes"] for cell in cells}),
    }
    if (
        len(cells) != 27
        or dimensions["frequencies_hz"] != [100, 500, 1000]
        or dimensions["payload_bytes"] != [64, 256, 1024]
        or dimensions["node_counts"] != [1, 4, 8]
        or any(cell["attempted"] != cell["succeeded"] for cell in cells)
    ):
        raise RuntimeError("performance matrix dimensions or success counts are invalid")
    report["platform"] = platform.platform()
    report["dimensions"] = dimensions
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "cells": len(cells),
                "dimensions": dimensions,
                "max_p99_us": max(cell["latency_p99_us"] for cell in cells),
                "max_cpu_percent": max(cell["cpu_percent"] for cell in cells),
                "max_rss_mib": max(cell["rss_mib"] for cell in cells),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()

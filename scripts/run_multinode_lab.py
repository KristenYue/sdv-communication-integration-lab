from __future__ import annotations

import argparse
import json
from pathlib import Path

from sdv_lab.multinode import run_multinode_lab


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-bin", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--generations", type=int, default=2)
    parser.add_argument("--frames-per-generation", type=int, default=30)
    args = parser.parse_args()
    report = run_multinode_lab(
        args.gateway_bin,
        generations=args.generations,
        frames_per_generation=args.frames_per_generation,
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

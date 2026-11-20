from __future__ import annotations

import argparse
import json
from pathlib import Path

from sdv_lab.system_runner import execute_system_corpus
from sdv_lab.system_scenarios import build_system_scenarios


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-bin", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    report = execute_system_corpus(args.gateway_bin, build_system_scenarios())
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if report["failed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

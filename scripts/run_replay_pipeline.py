from __future__ import annotations

import argparse
import json
from pathlib import Path

from sdv_lab.replay_runner import execute_replay_pipeline
from sdv_lab.replay_tasks import build_replay_tasks
from sdv_lab.system_scenarios import build_system_scenarios


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-bin", required=True)
    parser.add_argument("--output", default="artifacts/replay-report-v1.json")
    args = parser.parse_args()

    tasks = build_replay_tasks(build_system_scenarios(), 300)
    report = execute_replay_pipeline(args.gateway_bin, tasks)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

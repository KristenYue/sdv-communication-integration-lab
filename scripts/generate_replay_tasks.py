from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

from sdv_lab.replay_tasks import build_replay_tasks, replay_dataset_sha256
from sdv_lab.system_scenarios import build_system_scenarios


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="data/replay/tasks_v1.jsonl")
    parser.add_argument("--manifest", default="data/replay/tasks_v1.manifest.json")
    args = parser.parse_args()

    tasks = build_replay_tasks(build_system_scenarios(), 300)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(task.as_json() for task in tasks) + "\n", encoding="utf-8")
    manifest = {
        "schema_version": "sdv-replay-task-manifest-v1",
        "task_count": len(tasks),
        "unique_source_scenarios": len({task.source_scenario_id for task in tasks}),
        "generation_counts": dict(sorted(Counter(task.generation for task in tasks).items())),
        "sha256": replay_dataset_sha256(tasks),
        "data_boundary": "versioned_tasks_derived_from_synthetic_four_node_system_corpus",
    }
    manifest_path = Path(args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()

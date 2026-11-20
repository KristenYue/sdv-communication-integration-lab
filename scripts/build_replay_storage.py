from __future__ import annotations

import argparse
import json
import platform
from pathlib import Path

import pyarrow
from sdv_lab.replay_storage import write_replay_dataset
from sdv_lab.replay_tasks import build_replay_tasks
from sdv_lab.system_scenarios import build_system_scenarios


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--object-root", default="artifacts/replay-object-store")
    parser.add_argument("--metadata-db", default="artifacts/replay-storage.sqlite3")
    parser.add_argument("--output", default="artifacts/replay-storage-v1.json")
    args = parser.parse_args()

    tasks = build_replay_tasks(build_system_scenarios(), 300)
    dataset = write_replay_dataset(tasks, args.object_root, args.metadata_db)
    expected_rows = {"tasks": 300, "frames": 1300, "expected_states": 300}
    if not (
        dataset["task_count"] == 300
        and dataset["unique_source_scenarios"] == 200
        and dataset["row_counts"] == expected_rows
        and len(dataset["objects"]) == 3
        and dataset["readback_verified"]
    ):
        raise RuntimeError(f"replay storage acceptance failed: {dataset}")

    report = {
        "schema_version": "sdv-replay-storage-report-v1",
        "data_boundary": (
            "local_parquet_zstandard_content_addressed_files_and_sqlite_metadata;"
            "not_s3_or_postgresql_execution"
        ),
        "platform": platform.platform(),
        "pyarrow_version": pyarrow.__version__,
        "dataset": dataset,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

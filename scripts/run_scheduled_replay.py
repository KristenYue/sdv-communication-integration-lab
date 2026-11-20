from __future__ import annotations

import argparse
import json
import platform
from pathlib import Path

from sdv_lab.replay_tasks import build_replay_tasks
from sdv_lab.scheduled_replay import (
    execute_lease_control_experiments,
    execute_scheduled_replay,
)
from sdv_lab.system_scenarios import build_system_scenarios


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway-bin", required=True)
    parser.add_argument("--database-dir", default="artifacts/replay-scheduler")
    parser.add_argument("--output", default="artifacts/scheduled-replay-v1.json")
    args = parser.parse_args()

    database_dir = Path(args.database_dir)
    database_dir.mkdir(parents=True, exist_ok=True)
    jobs_database = database_dir / "jobs.sqlite3"
    controls_database = database_dir / "lease-controls.sqlite3"
    for path in (jobs_database, controls_database):
        path.unlink(missing_ok=True)
        Path(f"{path}-wal").unlink(missing_ok=True)
        Path(f"{path}-shm").unlink(missing_ok=True)

    tasks = build_replay_tasks(build_system_scenarios(), 300)
    batch = execute_scheduled_replay(args.gateway_bin, tasks, jobs_database, 4)
    controls = execute_lease_control_experiments(controls_database, 60)
    if not (
        batch["completed"] == 300
        and batch["expected_matches"] == 300
        and batch["commit_statuses"] == {"COMMITTED": 300}
        and batch["final_job_counts"] == {"pending": 0, "leased": 0, "completed": 300}
        and controls
        == {
            "experiments": 60,
            "heartbeats_accepted": 60,
            "premature_takeovers_blocked": 60,
            "takeover_commits": 60,
            "stale_commits_fenced": 60,
            "idempotent_commit_replays": 60,
            "result_conflicts_rejected": 60,
        }
    ):
        raise RuntimeError(f"scheduled replay acceptance failed: {batch=} {controls=}")
    report = {
        "schema_version": "sdv-scheduled-replay-v1",
        "data_boundary": "local_sqlite_metadata_four_workers_and_synthetic_vehicle_tasks",
        "platform": platform.platform(),
        "batch": batch,
        "lease_controls": controls,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

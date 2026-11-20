import json
from collections import Counter
from pathlib import Path

from sdv_lab.replay_tasks import build_replay_tasks, replay_dataset_sha256
from sdv_lab.system_scenarios import build_system_scenarios

ROOT = Path(__file__).resolve().parents[2]


def test_replay_task_inventory_is_versioned_and_unique():
    tasks = build_replay_tasks(build_system_scenarios(), 300)
    assert len(tasks) == 300
    assert len({task.task_id for task in tasks}) == 300
    assert len({task.source_scenario_id for task in tasks}) == 200
    assert Counter(task.generation for task in tasks) == {1: 200, 2: 100}


def test_checked_in_replay_tasks_match_generator():
    tasks = build_replay_tasks(build_system_scenarios(), 300)
    actual = (ROOT / "data" / "replay" / "tasks_v1.jsonl").read_text(
        encoding="utf-8"
    ).splitlines()
    manifest = json.loads(
        (ROOT / "data" / "replay" / "tasks_v1.manifest.json").read_text(encoding="utf-8")
    )
    assert actual == [task.as_json() for task in tasks]
    assert manifest["task_count"] == 300
    assert manifest["sha256"] == replay_dataset_sha256(tasks)

"""Versioned replay tasks derived from the frozen four-node vehicle corpus."""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass

from .system_scenarios import SystemScenario, system_dataset_sha256


@dataclass(frozen=True)
class ReplayTask:
    task_id: str
    source_scenario_id: str
    source_dataset_sha256: str
    generation: int
    scenario: SystemScenario

    def as_json(self) -> str:
        return json.dumps(asdict(self), ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def build_replay_tasks(scenarios: list[SystemScenario], count: int = 300) -> list[ReplayTask]:
    if not scenarios:
        raise ValueError("replay task generation requires at least one scenario")
    source_sha = system_dataset_sha256(scenarios)
    tasks = []
    for index in range(count):
        source_index = index % len(scenarios)
        generation = index // len(scenarios) + 1
        scenario = scenarios[source_index]
        tasks.append(
            ReplayTask(
                task_id=f"SDV-REPLAY-{index + 1:04d}",
                source_scenario_id=scenario.scenario_id,
                source_dataset_sha256=source_sha,
                generation=generation,
                scenario=scenario,
            )
        )
    return tasks


def replay_dataset_sha256(tasks: list[ReplayTask]) -> str:
    payload = "\n".join(task.as_json() for task in tasks).encode("utf-8") + b"\n"
    return hashlib.sha256(payload).hexdigest()

import json
from collections import Counter
from pathlib import Path

from sdv_lab.system_scenarios import build_system_scenarios, system_dataset_sha256

ROOT = Path(__file__).resolve().parents[2]


def test_system_corpus_has_required_size_and_fault_distribution():
    scenarios = build_system_scenarios()
    assert len(scenarios) == 200
    assert sum(item.fault_injected for item in scenarios) == 100
    assert sum(len(item.frames) for item in scenarios) == 900
    assert Counter(item.category for item in scenarios) == {
        "healthy_four_node": 100,
        "fault_duplicate_vcu": 20,
        "fault_stale_tpms": 20,
        "fault_invalid_dlc_bcm": 15,
        "fault_range_tbox": 15,
        "fault_unknown_identifier": 15,
        "fault_timestamp_conflict": 15,
    }


def test_checked_in_system_corpus_matches_generator():
    scenarios = build_system_scenarios()
    expected = [item.as_json() for item in scenarios]
    actual = (ROOT / "data" / "scenarios" / "system_v1.jsonl").read_text(
        encoding="utf-8"
    ).splitlines()
    manifest = json.loads(
        (ROOT / "data" / "scenarios" / "system_v1.manifest.json").read_text(
            encoding="utf-8"
        )
    )
    assert actual == expected
    assert manifest["sha256"] == system_dataset_sha256(scenarios)
    assert manifest["fault_injection_scenarios"] == 100

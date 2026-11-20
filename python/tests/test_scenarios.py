import json
from collections import Counter
from pathlib import Path

from sdv_lab import build_frozen_scenarios, dataset_sha256

ROOT = Path(__file__).resolve().parents[2]


def test_v1_corpus_is_frozen_and_balanced():
    scenarios = build_frozen_scenarios()
    assert len(scenarios) == 220
    assert sum(len(item.frames) for item in scenarios) == 300
    assert len({item.scenario_id for item in scenarios}) == 220
    assert Counter(item.category for item in scenarios) == {
        "happy_vehicle_status": 40,
        "happy_tire_pressure": 40,
        "invalid_dlc": 25,
        "unknown_identifier": 20,
        "duplicate_frame": 30,
        "stale_frame": 30,
        "timestamp_conflict": 20,
        "signal_out_of_range": 15,
    }


def test_v1_corpus_hash_is_stable():
    assert dataset_sha256(build_frozen_scenarios()) == (
        "e341b405adb001149c97446fe4f4cefb2ba8c2fd346bec1b8e6d05e15f9e5f55"
    )


def test_checked_in_corpus_and_manifest_match_generator():
    scenarios = build_frozen_scenarios()
    expected_lines = [item.as_json() for item in scenarios]
    actual_lines = (ROOT / "data" / "scenarios" / "v1.jsonl").read_text(
        encoding="utf-8"
    ).splitlines()
    manifest = json.loads(
        (ROOT / "data" / "scenarios" / "v1.manifest.json").read_text(encoding="utf-8")
    )
    assert actual_lines == expected_lines
    assert manifest["sha256"] == dataset_sha256(scenarios)
    assert manifest["scenario_count"] == len(scenarios)

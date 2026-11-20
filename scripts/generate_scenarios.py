from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

from sdv_lab import build_frozen_scenarios, dataset_sha256


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="data/scenarios/v1.jsonl")
    parser.add_argument("--manifest", default="data/scenarios/v1.manifest.json")
    args = parser.parse_args()

    scenarios = build_frozen_scenarios()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(item.as_json() for item in scenarios) + "\n", encoding="utf-8")
    manifest = {
        "schema_version": "sdv-frozen-scenarios-v1",
        "scenario_count": len(scenarios),
        "frame_count": sum(len(item.frames) for item in scenarios),
        "categories": dict(sorted(Counter(item.category for item in scenarios).items())),
        "sha256": dataset_sha256(scenarios),
        "data_boundary": "deterministic_synthetic_vehicle_frames",
    }
    manifest_path = Path(args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()

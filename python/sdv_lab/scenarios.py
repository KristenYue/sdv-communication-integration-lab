"""Versioned deterministic scenario corpus for the C++ gateway process."""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class ScenarioFrame:
    frame_id: int
    dlc: int
    data_hex: str
    bus_time_ms: int
    trace_id: str

    def cli_line(self) -> str:
        return (
            f"FRAME\t{self.frame_id}\t{self.dlc}\t{self.data_hex}\t"
            f"{self.bus_time_ms}\t{self.trace_id}"
        )


@dataclass(frozen=True)
class FrozenScenario:
    scenario_id: str
    category: str
    frames: tuple[ScenarioFrame, ...]
    expected_codes: tuple[str, ...]

    def as_json(self) -> str:
        return json.dumps(asdict(self), ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _status_payload(index: int, *, battery: int | None = None) -> str:
    speed_raw = 5_000 + index * 17
    battery_value = 50 + index % 50 if battery is None else battery
    door = index % 2
    payload = speed_raw.to_bytes(2, "little") + bytes([battery_value, door]) + bytes(4)
    return payload.hex()


def _tire_payload(index: int) -> str:
    values = [220 + index % 15, 221 + index % 15, 219 + index % 15, 222 + index % 15]
    return b"".join(value.to_bytes(2, "little") for value in values).hex()


def build_frozen_scenarios() -> list[FrozenScenario]:
    scenarios: list[FrozenScenario] = []

    def add(category: str, frames: list[ScenarioFrame], expected: list[str]) -> None:
        sequence = len(scenarios) + 1
        scenarios.append(
            FrozenScenario(
                scenario_id=f"SDV-{sequence:04d}",
                category=category,
                frames=tuple(frames),
                expected_codes=tuple(expected),
            )
        )

    for index in range(40):
        add(
            "happy_vehicle_status",
            [ScenarioFrame(256, 8, _status_payload(index), 1_000 + index, f"status-{index}")],
            ["APPLIED"],
        )
    for index in range(40):
        add(
            "happy_tire_pressure",
            [ScenarioFrame(257, 8, _tire_payload(index), 2_000 + index, f"tires-{index}")],
            ["APPLIED"],
        )
    for index in range(25):
        add(
            "invalid_dlc",
            [ScenarioFrame(256, 7, _status_payload(index), 3_000 + index, f"dlc-{index}")],
            ["INVALID_DLC"],
        )
    for index in range(20):
        add(
            "unknown_identifier",
            [ScenarioFrame(0x700 + index, 8, bytes(8).hex(), 4_000 + index, f"unknown-{index}")],
            ["UNKNOWN_CAN_ID"],
        )
    for index in range(30):
        frame = ScenarioFrame(256, 8, _status_payload(index), 5_000 + index, f"duplicate-{index}")
        add("duplicate_frame", [frame, frame], ["APPLIED", "DUPLICATE_FRAME"])
    for index in range(30):
        first = ScenarioFrame(257, 8, _tire_payload(index), 6_100 + index, f"stale-first-{index}")
        stale = ScenarioFrame(
            257, 8, _tire_payload(index + 1), 6_000 + index, f"stale-late-{index}"
        )
        add("stale_frame", [first, stale], ["APPLIED", "STALE_FRAME"])
    for index in range(20):
        timestamp = 7_000 + index
        first = ScenarioFrame(256, 8, _status_payload(index), timestamp, f"conflict-first-{index}")
        conflict = ScenarioFrame(
            256, 8, _status_payload(index + 1), timestamp, f"conflict-second-{index}"
        )
        add("timestamp_conflict", [first, conflict], ["APPLIED", "TIMESTAMP_CONFLICT"])
    for index in range(15):
        add(
            "signal_out_of_range",
            [
                ScenarioFrame(
                    256,
                    8,
                    _status_payload(index, battery=101),
                    8_000 + index,
                    f"range-{index}",
                )
            ],
            ["SIGNAL_OUT_OF_RANGE"],
        )

    if len(scenarios) != 220:
        raise AssertionError("the v1 corpus must contain exactly 220 scenarios")
    return scenarios


def dataset_sha256(scenarios: list[FrozenScenario]) -> str:
    payload = "\n".join(scenario.as_json() for scenario in scenarios).encode("utf-8") + b"\n"
    return hashlib.sha256(payload).hexdigest()

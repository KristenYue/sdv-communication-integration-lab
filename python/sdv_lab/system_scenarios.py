"""Frozen four-node system scenarios with fault-containment assertions."""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass

from .multinode import NODE_IDS, build_node_payload
from .scenarios import ScenarioFrame


@dataclass(frozen=True)
class SystemScenario:
    scenario_id: str
    category: str
    fault_injected: bool
    frames: tuple[ScenarioFrame, ...]
    expected_codes: tuple[str, ...]
    snapshot_now_ms: int
    expected_snapshot: dict

    def as_json(self) -> str:
        return json.dumps(asdict(self), ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _baseline(index: int, base_time: int) -> tuple[list[ScenarioFrame], dict]:
    frames = [
        ScenarioFrame(
            NODE_IDS[node],
            8,
            build_node_payload(node, index),
            base_time,
            f"system-{index}-{node}-baseline",
        )
        for node in ("vcu", "tpms", "bcm", "tbox")
    ]
    vcu = bytes.fromhex(frames[0].data_hex)
    tires = bytes.fromhex(frames[1].data_hex)
    bcm = bytes.fromhex(frames[2].data_hex)
    tbox = bytes.fromhex(frames[3].data_hex)
    expected = {
        "speed_kph": int.from_bytes(vcu[0:2], "little") * 0.01,
        "battery_percent": vcu[2],
        "door_open": bool(vcu[3] & 1),
        "tire_kpa": [int.from_bytes(tires[pos : pos + 2], "little") for pos in range(0, 8, 2)],
        "door_open_mask": bcm[0] & 0x0F,
        "door_lock_mask": bcm[1] & 0x0F,
        "wiper_state": bcm[2],
        "cloud_connected": tbox[0] == 1,
        "signal_quality_percent": tbox[1],
        "upload_backlog": int.from_bytes(tbox[2:4], "little"),
    }
    return frames, expected


def build_system_scenarios() -> list[SystemScenario]:
    scenarios: list[SystemScenario] = []

    def add(
        category: str,
        index: int,
        fault: ScenarioFrame | None,
        fault_code: str | None,
    ) -> None:
        base_time = 100_000 + index * 100
        frames, expected_snapshot = _baseline(index, base_time)
        codes = ["APPLIED"] * len(frames)
        if fault is not None and fault_code is not None:
            frames.append(fault)
            codes.append(fault_code)
        scenarios.append(
            SystemScenario(
                scenario_id=f"SDV-SYS-{len(scenarios) + 1:04d}",
                category=category,
                fault_injected=fault is not None,
                frames=tuple(frames),
                expected_codes=tuple(codes),
                snapshot_now_ms=base_time + 50,
                expected_snapshot=expected_snapshot,
            )
        )

    for index in range(100):
        add("healthy_four_node", index, None, None)

    offset = 100
    for local in range(20):
        index = offset + local
        base_time = 100_000 + index * 100
        fault = ScenarioFrame(
            256, 8, build_node_payload("vcu", index), base_time, f"fault-duplicate-{index}"
        )
        add("fault_duplicate_vcu", index, fault, "DUPLICATE_FRAME")
    offset += 20
    for local in range(20):
        index = offset + local
        base_time = 100_000 + index * 100
        fault = ScenarioFrame(
            257,
            8,
            build_node_payload("tpms", index + 1),
            base_time - 1,
            f"fault-stale-{index}",
        )
        add("fault_stale_tpms", index, fault, "STALE_FRAME")
    offset += 20
    for local in range(15):
        index = offset + local
        base_time = 100_000 + index * 100
        fault = ScenarioFrame(
            258, 7, build_node_payload("bcm", index + 1), base_time + 1, f"fault-dlc-{index}"
        )
        add("fault_invalid_dlc_bcm", index, fault, "INVALID_DLC")
    offset += 15
    for local in range(15):
        index = offset + local
        base_time = 100_000 + index * 100
        invalid_tbox = bytes([1, 101, 0, 0, 0, 0, 0, 0]).hex()
        fault = ScenarioFrame(259, 8, invalid_tbox, base_time + 1, f"fault-range-{index}")
        add("fault_range_tbox", index, fault, "SIGNAL_OUT_OF_RANGE")
    offset += 15
    for local in range(15):
        index = offset + local
        base_time = 100_000 + index * 100
        fault = ScenarioFrame(
            0x700 + local, 8, bytes(8).hex(), base_time + 1, f"fault-unknown-{index}"
        )
        add("fault_unknown_identifier", index, fault, "UNKNOWN_CAN_ID")
    offset += 15
    for local in range(15):
        index = offset + local
        base_time = 100_000 + index * 100
        fault = ScenarioFrame(
            258,
            8,
            build_node_payload("bcm", index + 1),
            base_time,
            f"fault-conflict-{index}",
        )
        add("fault_timestamp_conflict", index, fault, "TIMESTAMP_CONFLICT")

    if len(scenarios) != 200 or sum(item.fault_injected for item in scenarios) != 100:
        raise AssertionError("system corpus must contain 200 scenarios and 100 injected faults")
    return scenarios


def system_dataset_sha256(scenarios: list[SystemScenario]) -> str:
    payload = "\n".join(item.as_json() for item in scenarios).encode("utf-8") + b"\n"
    return hashlib.sha256(payload).hexdigest()

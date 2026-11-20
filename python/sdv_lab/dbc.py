"""Strict decoder for the project's unsigned little-endian classic-CAN DBC subset.

This intentionally rejects signed, Motorola-endian, multiplexed, overlapping, and
out-of-payload signals. It is not presented as a general DBC implementation.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Signal:
    name: str
    start: int
    width: int
    scale: float
    offset: float
    low: float
    high: float


@dataclass(frozen=True)
class Message:
    frame_id: int
    name: str
    size: int
    signals: tuple[Signal, ...]


class ProjectDBC:
    def __init__(self, path: str | Path):
        builders: dict[int, dict] = {}
        current: dict | None = None
        for raw in Path(path).read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if line.startswith("BO_ "):
                match = re.fullmatch(r"BO_ (\d+) (\w+): (\d+) \w+", line)
                if not match:
                    raise ValueError("unsupported DBC message")
                frame_id_text, name, size_text = match.groups()
                frame_id, size = int(frame_id_text), int(size_text)
                if not 0 <= frame_id <= 0x7FF or not 1 <= size <= 8:
                    raise ValueError("classic CAN standard frame required")
                if frame_id in builders:
                    raise ValueError("duplicate CAN identifier")
                current = {"frame_id": frame_id, "name": name, "size": size, "signals": []}
                builders[frame_id] = current
            elif line.startswith("SG_ "):
                match = re.fullmatch(
                    r'SG_ (\w+) : (\d+)\|(\d+)@1\+ \(([-\d.]+),([-\d.]+)\) '
                    r'\[([-\d.]+)\|([-\d.]+)\] "[^"]*" \w+',
                    line,
                )
                if not match or current is None:
                    raise ValueError("unsupported DBC signal format")
                name, start_text, width_text, scale, offset, low, high = match.groups()
                start, width = int(start_text), int(width_text)
                if width < 1 or start + width > current["size"] * 8:
                    raise ValueError("signal outside CAN payload")
                occupied = set(range(start, start + width))
                if any(occupied.intersection(range(s.start, s.start + s.width)) for s in current["signals"]):
                    raise ValueError("overlapping CAN signals")
                current["signals"].append(
                    Signal(name, start, width, float(scale), float(offset), float(low), float(high))
                )
        if not builders:
            raise ValueError("empty DBC")
        self.messages = {
            frame_id: Message(
                frame_id=frame_id,
                name=builder["name"],
                size=builder["size"],
                signals=tuple(builder["signals"]),
            )
            for frame_id, builder in builders.items()
        }

    def decode(self, frame_id: int, payload: bytes) -> dict:
        if frame_id not in self.messages:
            raise ValueError("unknown CAN identifier")
        message = self.messages[frame_id]
        if not isinstance(payload, bytes) or len(payload) != message.size:
            raise ValueError("invalid CAN DLC")
        bits = int.from_bytes(payload, "little")
        values = {}
        for signal in message.signals:
            raw = (bits >> signal.start) & ((1 << signal.width) - 1)
            value = raw * signal.scale + signal.offset
            if not signal.low <= value <= signal.high:
                raise ValueError(f"signal out of declared range: {signal.name}")
            values[signal.name] = value
        return {
            "frame_id": frame_id,
            "message": message.name,
            "signals": values,
            "data_boundary": "synthetic_or_vcan_frame",
            "decoder": "project_dbc_subset",
        }


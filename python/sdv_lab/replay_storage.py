"""Normalized Parquet storage for versioned vehicle replay tasks."""

from __future__ import annotations

import hashlib
import os
import sqlite3
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from .replay_tasks import ReplayTask, replay_dataset_sha256

SCHEMA_VERSION = "sdv-replay-storage-v1"

TASK_SCHEMA = pa.schema(
    [
        pa.field("task_id", pa.string(), nullable=False),
        pa.field("source_scenario_id", pa.string(), nullable=False),
        pa.field("source_dataset_sha256", pa.string(), nullable=False),
        pa.field("generation", pa.int16(), nullable=False),
        pa.field("category", pa.string(), nullable=False),
        pa.field("fault_injected", pa.bool_(), nullable=False),
        pa.field("snapshot_now_ms", pa.int64(), nullable=False),
    ]
)

FRAME_SCHEMA = pa.schema(
    [
        pa.field("task_id", pa.string(), nullable=False),
        pa.field("ordinal", pa.int16(), nullable=False),
        pa.field("frame_id", pa.uint32(), nullable=False),
        pa.field("dlc", pa.uint8(), nullable=False),
        pa.field("payload", pa.binary(8), nullable=False),
        pa.field("bus_time_ms", pa.int64(), nullable=False),
        pa.field("trace_id", pa.string(), nullable=False),
        pa.field("expected_code", pa.string(), nullable=False),
    ]
)

STATE_SCHEMA = pa.schema(
    [
        pa.field("task_id", pa.string(), nullable=False),
        pa.field("speed_kph", pa.float64(), nullable=False),
        pa.field("battery_percent", pa.int16(), nullable=False),
        pa.field("door_open", pa.bool_(), nullable=False),
        pa.field("tire_fl_kpa", pa.int16(), nullable=False),
        pa.field("tire_fr_kpa", pa.int16(), nullable=False),
        pa.field("tire_rl_kpa", pa.int16(), nullable=False),
        pa.field("tire_rr_kpa", pa.int16(), nullable=False),
        pa.field("door_open_mask", pa.uint8(), nullable=False),
        pa.field("door_lock_mask", pa.uint8(), nullable=False),
        pa.field("wiper_state", pa.uint8(), nullable=False),
        pa.field("cloud_connected", pa.bool_(), nullable=False),
        pa.field("signal_quality_percent", pa.int16(), nullable=False),
        pa.field("upload_backlog", pa.int32(), nullable=False),
    ]
)


@dataclass(frozen=True)
class StoredObject:
    logical_name: str
    sha256: str
    size_bytes: int
    row_count: int
    schema_version: str
    uri: str


def build_arrow_tables(tasks: list[ReplayTask]) -> dict[str, pa.Table]:
    task_rows: list[dict] = []
    frame_rows: list[dict] = []
    state_rows: list[dict] = []
    for task in tasks:
        scenario = task.scenario
        task_rows.append(
            {
                "task_id": task.task_id,
                "source_scenario_id": task.source_scenario_id,
                "source_dataset_sha256": task.source_dataset_sha256,
                "generation": task.generation,
                "category": scenario.category,
                "fault_injected": scenario.fault_injected,
                "snapshot_now_ms": scenario.snapshot_now_ms,
            }
        )
        for ordinal, (frame, expected_code) in enumerate(
            zip(scenario.frames, scenario.expected_codes, strict=True)
        ):
            payload = bytes.fromhex(frame.data_hex)
            if len(payload) != 8:
                raise ValueError(f"{task.task_id} frame {ordinal} does not contain 8 bytes")
            frame_rows.append(
                {
                    "task_id": task.task_id,
                    "ordinal": ordinal,
                    "frame_id": frame.frame_id,
                    "dlc": frame.dlc,
                    "payload": payload,
                    "bus_time_ms": frame.bus_time_ms,
                    "trace_id": frame.trace_id,
                    "expected_code": expected_code,
                }
            )
        state = scenario.expected_snapshot
        tires = state["tire_kpa"]
        if len(tires) != 4:
            raise ValueError(f"{task.task_id} expected state must contain four tires")
        state_rows.append(
            {
                "task_id": task.task_id,
                "speed_kph": state["speed_kph"],
                "battery_percent": state["battery_percent"],
                "door_open": state["door_open"],
                "tire_fl_kpa": tires[0],
                "tire_fr_kpa": tires[1],
                "tire_rl_kpa": tires[2],
                "tire_rr_kpa": tires[3],
                "door_open_mask": state["door_open_mask"],
                "door_lock_mask": state["door_lock_mask"],
                "wiper_state": state["wiper_state"],
                "cloud_connected": state["cloud_connected"],
                "signal_quality_percent": state["signal_quality_percent"],
                "upload_backlog": state["upload_backlog"],
            }
        )
    return {
        "tasks": pa.Table.from_pylist(task_rows, schema=TASK_SCHEMA),
        "frames": pa.Table.from_pylist(frame_rows, schema=FRAME_SCHEMA),
        "expected_states": pa.Table.from_pylist(state_rows, schema=STATE_SCHEMA),
    }


class ReplayObjectStore:
    """Local content-addressed objects plus transactional SQLite metadata."""

    def __init__(self, object_root: str | Path, metadata_database: str | Path):
        self.object_root = Path(object_root)
        self.metadata_database = Path(metadata_database)
        self.object_root.mkdir(parents=True, exist_ok=True)
        self.metadata_database.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as connection:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS replay_objects (
                    logical_name TEXT PRIMARY KEY,
                    sha256 TEXT NOT NULL,
                    size_bytes INTEGER NOT NULL CHECK(size_bytes > 0),
                    row_count INTEGER NOT NULL CHECK(row_count >= 0),
                    schema_version TEXT NOT NULL,
                    uri TEXT NOT NULL UNIQUE
                )
                """
            )

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.metadata_database, timeout=5)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA busy_timeout=5000")
        return connection

    def _content_path(self, sha256: str) -> Path:
        return self.object_root / "sha256" / sha256[:2] / f"{sha256}.parquet"

    def put_table(self, logical_name: str, table: pa.Table) -> StoredObject:
        temp_fd, temp_name = tempfile.mkstemp(suffix=".parquet", dir=self.object_root)
        os.close(temp_fd)
        temp_path = Path(temp_name)
        try:
            pq.write_table(table, temp_path, compression="zstd")
            payload = temp_path.read_bytes()
            digest = hashlib.sha256(payload).hexdigest()
            content_path = self._content_path(digest)
            content_path.parent.mkdir(parents=True, exist_ok=True)
            if content_path.exists():
                if hashlib.sha256(content_path.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(f"content-addressed object is corrupt: {content_path}")
                temp_path.unlink()
            else:
                temp_path.replace(content_path)
            uri = content_path.relative_to(self.object_root).as_posix()
            stored = StoredObject(
                logical_name=logical_name,
                sha256=digest,
                size_bytes=content_path.stat().st_size,
                row_count=table.num_rows,
                schema_version=SCHEMA_VERSION,
                uri=uri,
            )
            with self._connect() as connection:
                existing = connection.execute(
                    "SELECT * FROM replay_objects WHERE logical_name=?", (logical_name,)
                ).fetchone()
                if existing is not None:
                    current = StoredObject(**dict(existing))
                    if current != stored:
                        raise RuntimeError(f"logical object conflict: {logical_name}")
                    return current
                connection.execute(
                    """
                    INSERT INTO replay_objects
                    (logical_name, sha256, size_bytes, row_count, schema_version, uri)
                    VALUES (?, ?, ?, ?, ?, ?)
                    """,
                    tuple(asdict(stored).values()),
                )
            return stored
        finally:
            temp_path.unlink(missing_ok=True)

    def read_table(self, logical_name: str, expected_schema: pa.Schema) -> pa.Table:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT * FROM replay_objects WHERE logical_name=?", (logical_name,)
            ).fetchone()
        if row is None:
            raise KeyError(logical_name)
        stored = StoredObject(**dict(row))
        root = self.object_root.resolve()
        path = (self.object_root / stored.uri).resolve()
        if root not in path.parents:
            raise RuntimeError(f"object URI escapes object root: {logical_name}")
        payload = path.read_bytes()
        if hashlib.sha256(payload).hexdigest() != stored.sha256:
            raise RuntimeError(f"object hash mismatch: {logical_name}")
        if pq.read_schema(path) != expected_schema:
            raise RuntimeError(f"object schema mismatch: {logical_name}")
        table = pq.read_table(path)
        if table.schema != expected_schema or table.num_rows != stored.row_count:
            raise RuntimeError(f"object schema or row count mismatch: {logical_name}")
        return table

    def objects(self) -> list[StoredObject]:
        with self._connect() as connection:
            rows = connection.execute(
                "SELECT * FROM replay_objects ORDER BY logical_name"
            ).fetchall()
        return [StoredObject(**dict(row)) for row in rows]


def write_replay_dataset(
    tasks: list[ReplayTask], object_root: str | Path, metadata_database: str | Path
) -> dict:
    tables = build_arrow_tables(tasks)
    store = ReplayObjectStore(object_root, metadata_database)
    schemas = {
        "tasks": TASK_SCHEMA,
        "frames": FRAME_SCHEMA,
        "expected_states": STATE_SCHEMA,
    }
    stored = [store.put_table(name, table) for name, table in tables.items()]
    for name, schema in schemas.items():
        readback = store.read_table(name, schema)
        if not readback.equals(tables[name]):
            raise RuntimeError(f"Parquet readback differs from source table: {name}")
    return {
        "schema_version": SCHEMA_VERSION,
        "logical_dataset_sha256": replay_dataset_sha256(tasks),
        "task_count": len(tasks),
        "unique_source_scenarios": len({task.source_scenario_id for task in tasks}),
        "row_counts": {name: table.num_rows for name, table in tables.items()},
        "objects": [asdict(item) for item in sorted(stored, key=lambda item: item.logical_name)],
        "total_stored_bytes": sum(item.size_bytes for item in stored),
        "readback_verified": True,
    }

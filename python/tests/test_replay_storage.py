from __future__ import annotations

import sqlite3

from sdv_lab.replay_storage import (
    FRAME_SCHEMA,
    STATE_SCHEMA,
    TASK_SCHEMA,
    ReplayObjectStore,
    build_arrow_tables,
    write_replay_dataset,
)
from sdv_lab.replay_tasks import build_replay_tasks
from sdv_lab.system_scenarios import build_system_scenarios


def _tasks(count: int = 300):
    return build_replay_tasks(build_system_scenarios(), count)


def test_build_arrow_tables_has_explicit_schemas_and_rows() -> None:
    tables = build_arrow_tables(_tasks())
    assert tables["tasks"].schema == TASK_SCHEMA
    assert tables["frames"].schema == FRAME_SCHEMA
    assert tables["expected_states"].schema == STATE_SCHEMA
    assert {name: table.num_rows for name, table in tables.items()} == {
        "tasks": 300,
        "frames": 1300,
        "expected_states": 300,
    }


def test_write_readback_and_idempotent_registration(tmp_path) -> None:
    object_root = tmp_path / "objects"
    metadata = tmp_path / "metadata.sqlite3"
    tasks = _tasks(12)
    first = write_replay_dataset(tasks, object_root, metadata)
    second = write_replay_dataset(tasks, object_root, metadata)
    assert first == second
    assert first["readback_verified"] is True
    assert len(first["objects"]) == 3

    store = ReplayObjectStore(object_root, metadata)
    for item in store.objects():
        path = object_root / item.uri
        assert path.exists()
        assert path.name == f"{item.sha256}.parquet"
        assert path.parent.name == item.sha256[:2]
    with sqlite3.connect(metadata) as connection:
        assert connection.execute("SELECT COUNT(*) FROM replay_objects").fetchone()[0] == 3


def test_read_rejects_content_tampering(tmp_path) -> None:
    object_root = tmp_path / "objects"
    metadata = tmp_path / "metadata.sqlite3"
    write_replay_dataset(_tasks(2), object_root, metadata)
    store = ReplayObjectStore(object_root, metadata)
    target = next(item for item in store.objects() if item.logical_name == "tasks")
    path = object_root / target.uri
    path.write_bytes(path.read_bytes() + b"corruption")
    try:
        store.read_table("tasks", TASK_SCHEMA)
    except RuntimeError as error:
        assert "hash mismatch" in str(error)
    else:
        raise AssertionError("tampered object must be rejected")

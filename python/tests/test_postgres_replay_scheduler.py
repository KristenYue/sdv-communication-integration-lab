from __future__ import annotations

import pytest
from sdv_lab.postgres_replay_scheduler import PostgresReplayJobStore


@pytest.mark.parametrize(
    "invalid_digest",
    ["short", "g" * 64, "A" * 64, "0" * 63, "0" * 65],
)
def test_postgres_store_rejects_noncanonical_dataset_digest(invalid_digest: str) -> None:
    with pytest.raises(ValueError, match="lowercase hexadecimal SHA-256"):
        PostgresReplayJobStore("postgresql://unused", invalid_digest, "v1", 1)


def test_postgres_store_accepts_canonical_identity_without_connecting() -> None:
    store = PostgresReplayJobStore("postgresql://unused", "0" * 64, "v1", 1)
    assert store.dataset_sha256 == "0" * 64


@pytest.mark.parametrize("worker_id, lease_ms", [("", 1), ("worker", 0), ("worker", -1)])
def test_postgres_store_rejects_invalid_lease_inputs(worker_id: str, lease_ms: int) -> None:
    store = PostgresReplayJobStore("postgresql://unused", "0" * 64, "v1", 1)
    with pytest.raises(ValueError):
        store._validate_lease(worker_id, lease_ms)


def test_seed_uses_cursor_batch_and_verifies_inventory(monkeypatch) -> None:
    class Result:
        def __init__(self, one=None, many=None):
            self.one = one
            self.many = many

        def fetchone(self):
            return self.one

        def fetchall(self):
            return self.many

    class Cursor:
        def __init__(self):
            self.rows = None

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def executemany(self, _sql, rows):
            self.rows = rows

    class Connection:
        def __init__(self):
            self.batch_cursor = Cursor()

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def cursor(self):
            return self.batch_cursor

        def execute(self, sql, _parameters):
            if "SELECT schema_version" in sql:
                return Result(
                    one={"schema_version": "v1", "task_count": 2, "unique_source_scenarios": 2}
                )
            if "SELECT task_id" in sql:
                return Result(many=[{"task_id": "task-a"}, {"task_id": "task-b"}])
            return Result()

    connection = Connection()
    store = PostgresReplayJobStore("postgresql://unused", "0" * 64, "v1", 2)
    monkeypatch.setattr(store, "_connect", lambda: connection)
    store.seed(["task-b", "task-a"])
    assert connection.batch_cursor.rows == [
        ("0" * 64, "task-b"),
        ("0" * 64, "task-a"),
    ]

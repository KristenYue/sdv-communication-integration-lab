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

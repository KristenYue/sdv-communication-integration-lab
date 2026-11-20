from sdv_lab.replay_scheduler import ReplayJobStore


def test_heartbeat_blocks_premature_takeover(tmp_path):
    store = ReplayJobStore(tmp_path / "jobs.sqlite3")
    store.seed(["task-1"])
    first = store.claim_task("task-1", "worker-a", 0, 10)
    assert first is not None
    assert store.heartbeat(first, 5, 10)
    assert store.claim_task("task-1", "worker-b", 11, 10) is None
    second = store.claim_task("task-1", "worker-b", 16, 10)
    assert second is not None
    assert second.fencing_token == first.fencing_token + 1


def test_stale_worker_is_fenced_and_winner_is_idempotent(tmp_path):
    store = ReplayJobStore(tmp_path / "jobs.sqlite3")
    store.seed(["task-1"])
    first = store.claim_task("task-1", "worker-a", 0, 10)
    second = store.claim_task("task-1", "worker-b", 11, 10)
    assert first is not None and second is not None
    assert store.commit(second, "result-a", True, 12) == "COMMITTED"
    assert store.commit(first, "result-a", True, 12) == "FENCED"
    assert store.commit(second, "result-a", True, 12) == "IDEMPOTENT_REPLAY"
    assert store.commit(second, "result-b", True, 12) == "RESULT_CONFLICT"


def test_acquire_next_completes_inventory(tmp_path):
    store = ReplayJobStore(tmp_path / "jobs.sqlite3")
    store.seed(["task-2", "task-1"])
    first = store.acquire_next("worker", 0, 10)
    assert first is not None and first.task_id == "task-1"
    assert store.commit(first, "sha-1", True, 1) == "COMMITTED"
    second = store.acquire_next("worker", 1, 10)
    assert second is not None and second.task_id == "task-2"
    assert store.commit(second, "sha-2", True, 2) == "COMMITTED"
    assert store.acquire_next("worker", 2, 10) is None
    assert store.counts() == {"pending": 0, "leased": 0, "completed": 2}

"""SQLite-backed replay job leases with heartbeat, fencing, and idempotent commit."""

from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ReplayLease:
    task_id: str
    worker_id: str
    fencing_token: int
    lease_expires_ms: int
    attempt: int


class ReplayJobStore:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as connection:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS replay_jobs (
                    task_id TEXT PRIMARY KEY,
                    status TEXT NOT NULL CHECK(status IN ('pending', 'leased', 'completed')),
                    lease_owner TEXT,
                    lease_expires_ms INTEGER,
                    fencing_token INTEGER NOT NULL DEFAULT 0,
                    attempt INTEGER NOT NULL DEFAULT 0,
                    result_sha256 TEXT,
                    expected_match INTEGER,
                    updated_ms INTEGER NOT NULL
                )
                """
            )

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=5, isolation_level=None)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA busy_timeout=5000")
        return connection

    def seed(self, task_ids: list[str], now_ms: int = 0) -> None:
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.executemany(
                """
                INSERT OR IGNORE INTO replay_jobs(task_id, status, updated_ms)
                VALUES (?, 'pending', ?)
                """,
                [(task_id, now_ms) for task_id in task_ids],
            )
            connection.commit()

    @staticmethod
    def _lease_from_row(row: sqlite3.Row, worker_id: str, now_ms: int, lease_ms: int) -> ReplayLease:
        return ReplayLease(
            task_id=str(row["task_id"]),
            worker_id=worker_id,
            fencing_token=int(row["fencing_token"]) + 1,
            lease_expires_ms=now_ms + lease_ms,
            attempt=int(row["attempt"]) + 1,
        )

    def acquire_next(self, worker_id: str, now_ms: int, lease_ms: int) -> ReplayLease | None:
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                """
                SELECT task_id, fencing_token, attempt
                FROM replay_jobs
                WHERE status = 'pending'
                   OR (status = 'leased' AND lease_expires_ms <= ?)
                ORDER BY task_id
                LIMIT 1
                """,
                (now_ms,),
            ).fetchone()
            if row is None:
                connection.commit()
                return None
            lease = self._lease_from_row(row, worker_id, now_ms, lease_ms)
            connection.execute(
                """
                UPDATE replay_jobs
                SET status='leased', lease_owner=?, lease_expires_ms=?, fencing_token=?,
                    attempt=?, updated_ms=?
                WHERE task_id=?
                """,
                (
                    worker_id,
                    lease.lease_expires_ms,
                    lease.fencing_token,
                    lease.attempt,
                    now_ms,
                    lease.task_id,
                ),
            )
            connection.commit()
            return lease

    def claim_task(
        self, task_id: str, worker_id: str, now_ms: int, lease_ms: int
    ) -> ReplayLease | None:
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                """
                SELECT task_id, status, lease_expires_ms, fencing_token, attempt
                FROM replay_jobs WHERE task_id=?
                """,
                (task_id,),
            ).fetchone()
            if row is None or row["status"] == "completed":
                connection.commit()
                return None
            if row["status"] == "leased" and int(row["lease_expires_ms"]) > now_ms:
                connection.commit()
                return None
            lease = self._lease_from_row(row, worker_id, now_ms, lease_ms)
            connection.execute(
                """
                UPDATE replay_jobs
                SET status='leased', lease_owner=?, lease_expires_ms=?, fencing_token=?,
                    attempt=?, updated_ms=? WHERE task_id=?
                """,
                (
                    worker_id,
                    lease.lease_expires_ms,
                    lease.fencing_token,
                    lease.attempt,
                    now_ms,
                    task_id,
                ),
            )
            connection.commit()
            return lease

    def heartbeat(self, lease: ReplayLease, now_ms: int, lease_ms: int) -> bool:
        with self._connect() as connection:
            cursor = connection.execute(
                """
                UPDATE replay_jobs
                SET lease_expires_ms=?, updated_ms=?
                WHERE task_id=? AND status='leased' AND lease_owner=?
                  AND fencing_token=? AND lease_expires_ms > ?
                """,
                (
                    now_ms + lease_ms,
                    now_ms,
                    lease.task_id,
                    lease.worker_id,
                    lease.fencing_token,
                    now_ms,
                ),
            )
            return cursor.rowcount == 1

    def commit(
        self,
        lease: ReplayLease,
        result_sha256: str,
        expected_match: bool,
        now_ms: int,
    ) -> str:
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                """
                SELECT status, lease_owner, lease_expires_ms, fencing_token, result_sha256,
                       expected_match
                FROM replay_jobs WHERE task_id=?
                """,
                (lease.task_id,),
            ).fetchone()
            if row is None:
                connection.commit()
                return "TASK_NOT_FOUND"
            if int(row["fencing_token"]) != lease.fencing_token or row["lease_owner"] != lease.worker_id:
                connection.commit()
                return "FENCED"
            if row["status"] == "completed":
                same = row["result_sha256"] == result_sha256 and bool(
                    row["expected_match"]
                ) == expected_match
                connection.commit()
                return "IDEMPOTENT_REPLAY" if same else "RESULT_CONFLICT"
            if int(row["lease_expires_ms"]) <= now_ms:
                connection.commit()
                return "LEASE_EXPIRED"
            connection.execute(
                """
                UPDATE replay_jobs
                SET status='completed', result_sha256=?, expected_match=?, updated_ms=?
                WHERE task_id=?
                """,
                (result_sha256, int(expected_match), now_ms, lease.task_id),
            )
            connection.commit()
            return "COMMITTED"

    def counts(self) -> dict[str, int]:
        with self._connect() as connection:
            rows = connection.execute(
                "SELECT status, COUNT(*) AS count FROM replay_jobs GROUP BY status"
            ).fetchall()
        result = {"pending": 0, "leased": 0, "completed": 0}
        result.update({str(row["status"]): int(row["count"]) for row in rows})
        return result

    def completed_results(self) -> list[tuple[str, str, bool]]:
        with self._connect() as connection:
            rows = connection.execute(
                """
                SELECT task_id, result_sha256, expected_match FROM replay_jobs
                WHERE status='completed' ORDER BY task_id
                """
            ).fetchall()
        return [
            (str(row["task_id"]), str(row["result_sha256"]), bool(row["expected_match"]))
            for row in rows
        ]

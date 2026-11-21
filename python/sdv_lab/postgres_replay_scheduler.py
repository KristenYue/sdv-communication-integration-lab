"""PostgreSQL replay leases using server time and row-level fencing."""

from __future__ import annotations

import re
from pathlib import Path

try:
    import psycopg
    from psycopg.rows import dict_row
except ModuleNotFoundError:  # pragma: no cover - exercised by the optional dependency boundary
    psycopg = None
    dict_row = None

from .replay_scheduler import ReplayLease


def _require_driver() -> None:
    if psycopg is None:
        raise RuntimeError("PostgreSQL support requires: pip install -e '.[postgres]'")


def apply_migration(dsn: str, migration: str | Path) -> None:
    _require_driver()
    sql = Path(migration).read_text(encoding="utf-8")
    statements = [statement.strip() for statement in sql.split(";") if statement.strip()]
    with psycopg.connect(dsn) as connection:
        for statement in statements:
            connection.execute(statement)


class PostgresReplayJobStore:
    """Dataset-scoped store; all lease decisions use PostgreSQL clock_timestamp()."""

    def __init__(
        self,
        dsn: str,
        dataset_sha256: str,
        schema_version: str,
        unique_source_scenarios: int,
    ):
        _require_driver()
        if re.fullmatch(r"[0-9a-f]{64}", dataset_sha256) is None:
            raise ValueError("dataset_sha256 must be a lowercase hexadecimal SHA-256")
        self.dsn = dsn
        self.dataset_sha256 = dataset_sha256
        self.schema_version = schema_version
        self.unique_source_scenarios = unique_source_scenarios

    def _connect(self):
        return psycopg.connect(self.dsn, row_factory=dict_row)

    @staticmethod
    def _validate_lease(worker_id: str, lease_ms: int) -> None:
        if not worker_id:
            raise ValueError("worker_id must not be empty")
        if lease_ms <= 0:
            raise ValueError("lease_ms must be positive")

    def server_info(self) -> dict[str, str]:
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT current_database() AS database,
                       current_setting('server_version') AS server_version,
                       current_setting('server_version_num') AS server_version_num
                """
            ).fetchone()
        return {key: str(value) for key, value in row.items()}

    @staticmethod
    def _lease(row: dict) -> ReplayLease:
        return ReplayLease(
            task_id=str(row["task_id"]),
            worker_id=str(row["lease_owner"]),
            fencing_token=int(row["fencing_token"]),
            lease_expires_ms=int(row["lease_expires_ms"]),
            attempt=int(row["attempt"]),
        )

    def seed(self, task_ids: list[str], now_ms: int = 0) -> None:
        del now_ms  # PostgreSQL is the sole lease clock.
        if len(set(task_ids)) != len(task_ids):
            raise ValueError("task IDs must be unique")
        with self._connect() as connection:
            connection.execute(
                """
                INSERT INTO replay_datasets
                    (dataset_sha256, schema_version, task_count, unique_source_scenarios)
                VALUES (%s, %s, %s, %s)
                ON CONFLICT (dataset_sha256) DO NOTHING
                """,
                (
                    self.dataset_sha256,
                    self.schema_version,
                    len(task_ids),
                    self.unique_source_scenarios,
                ),
            )
            dataset = connection.execute(
                """
                SELECT schema_version, task_count, unique_source_scenarios
                FROM replay_datasets WHERE dataset_sha256=%s
                """,
                (self.dataset_sha256,),
            ).fetchone()
            expected = {
                "schema_version": self.schema_version,
                "task_count": len(task_ids),
                "unique_source_scenarios": self.unique_source_scenarios,
            }
            if dataset != expected:
                raise RuntimeError("dataset identity conflicts with existing PostgreSQL metadata")
            connection.executemany(
                """
                INSERT INTO replay_jobs(dataset_sha256, task_id)
                VALUES (%s, %s)
                ON CONFLICT (dataset_sha256, task_id) DO NOTHING
                """,
                [(self.dataset_sha256, task_id) for task_id in task_ids],
            )
            stored_task_ids = [
                str(row["task_id"])
                for row in connection.execute(
                    """
                    SELECT task_id FROM replay_jobs
                    WHERE dataset_sha256=%s ORDER BY task_id
                    """,
                    (self.dataset_sha256,),
                ).fetchall()
            ]
            if stored_task_ids != sorted(task_ids):
                raise RuntimeError("task inventory conflicts with existing PostgreSQL metadata")

    def acquire_next(
        self, worker_id: str, now_ms: int, lease_ms: int
    ) -> ReplayLease | None:
        del now_ms
        self._validate_lease(worker_id, lease_ms)
        with self._connect() as connection:
            row = connection.execute(
                """
                WITH candidate AS (
                    SELECT dataset_sha256, task_id
                    FROM replay_jobs
                    WHERE dataset_sha256=%s
                      AND (status='pending' OR
                           (status='leased' AND lease_expires_at <= clock_timestamp()))
                    ORDER BY task_id
                    FOR UPDATE SKIP LOCKED
                    LIMIT 1
                )
                UPDATE replay_jobs AS job
                SET status='leased', lease_owner=%s,
                    lease_expires_at=clock_timestamp() + (%s * interval '1 millisecond'),
                    fencing_token=job.fencing_token + 1,
                    attempt=job.attempt + 1, updated_at=clock_timestamp()
                FROM candidate
                WHERE job.dataset_sha256=candidate.dataset_sha256
                  AND job.task_id=candidate.task_id
                RETURNING job.task_id, job.lease_owner, job.fencing_token, job.attempt,
                    (EXTRACT(EPOCH FROM job.lease_expires_at) * 1000)::bigint
                        AS lease_expires_ms
                """,
                (self.dataset_sha256, worker_id, lease_ms),
            ).fetchone()
            return None if row is None else self._lease(row)

    def claim_task(
        self, task_id: str, worker_id: str, now_ms: int, lease_ms: int
    ) -> ReplayLease | None:
        del now_ms
        self._validate_lease(worker_id, lease_ms)
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT status, lease_expires_at
                FROM replay_jobs
                WHERE dataset_sha256=%s AND task_id=%s
                FOR UPDATE
                """,
                (self.dataset_sha256, task_id),
            ).fetchone()
            if row is None or row["status"] == "completed":
                return None
            available = row["status"] == "pending" or connection.execute(
                "SELECT %s <= clock_timestamp() AS expired", (row["lease_expires_at"],)
            ).fetchone()["expired"]
            if not available:
                return None
            claimed = connection.execute(
                """
                UPDATE replay_jobs
                SET status='leased', lease_owner=%s,
                    lease_expires_at=clock_timestamp() + (%s * interval '1 millisecond'),
                    fencing_token=fencing_token + 1, attempt=attempt + 1,
                    updated_at=clock_timestamp()
                WHERE dataset_sha256=%s AND task_id=%s
                RETURNING task_id, lease_owner, fencing_token, attempt,
                    (EXTRACT(EPOCH FROM lease_expires_at) * 1000)::bigint
                        AS lease_expires_ms
                """,
                (worker_id, lease_ms, self.dataset_sha256, task_id),
            ).fetchone()
            return self._lease(claimed)

    def heartbeat(self, lease: ReplayLease, now_ms: int, lease_ms: int) -> bool:
        del now_ms
        self._validate_lease(lease.worker_id, lease_ms)
        with self._connect() as connection:
            cursor = connection.execute(
                """
                UPDATE replay_jobs
                SET lease_expires_at=clock_timestamp() + (%s * interval '1 millisecond'),
                    updated_at=clock_timestamp()
                WHERE dataset_sha256=%s AND task_id=%s AND status='leased'
                  AND lease_owner=%s AND fencing_token=%s
                  AND lease_expires_at > clock_timestamp()
                """,
                (
                    lease_ms,
                    self.dataset_sha256,
                    lease.task_id,
                    lease.worker_id,
                    lease.fencing_token,
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
        del now_ms
        if re.fullmatch(r"[0-9a-f]{64}", result_sha256) is None:
            raise ValueError("result_sha256 must be a lowercase hexadecimal SHA-256")
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT status, lease_owner, lease_expires_at, fencing_token,
                       result_sha256, expected_match,
                       lease_expires_at <= clock_timestamp() AS lease_expired
                FROM replay_jobs
                WHERE dataset_sha256=%s AND task_id=%s
                FOR UPDATE
                """,
                (self.dataset_sha256, lease.task_id),
            ).fetchone()
            if row is None:
                return "TASK_NOT_FOUND"
            if int(row["fencing_token"]) != lease.fencing_token or row["lease_owner"] != lease.worker_id:
                return "FENCED"
            if row["status"] == "completed":
                same = (
                    row["result_sha256"] == result_sha256
                    and bool(row["expected_match"]) == expected_match
                )
                return "IDEMPOTENT_REPLAY" if same else "RESULT_CONFLICT"
            if row["lease_expired"]:
                return "LEASE_EXPIRED"
            connection.execute(
                """
                UPDATE replay_jobs
                SET status='completed', result_sha256=%s, expected_match=%s,
                    updated_at=clock_timestamp()
                WHERE dataset_sha256=%s AND task_id=%s
                """,
                (result_sha256, expected_match, self.dataset_sha256, lease.task_id),
            )
            return "COMMITTED"

    def counts(self) -> dict[str, int]:
        with self._connect() as connection:
            rows = connection.execute(
                """
                SELECT status::text AS status, COUNT(*) AS count
                FROM replay_jobs WHERE dataset_sha256=%s GROUP BY status
                """,
                (self.dataset_sha256,),
            ).fetchall()
        result = {"pending": 0, "leased": 0, "completed": 0}
        result.update({str(row["status"]): int(row["count"]) for row in rows})
        return result

    def completed_results(self) -> list[tuple[str, str, bool]]:
        with self._connect() as connection:
            rows = connection.execute(
                """
                SELECT task_id, result_sha256, expected_match
                FROM replay_jobs
                WHERE dataset_sha256=%s AND status='completed'
                ORDER BY task_id
                """,
                (self.dataset_sha256,),
            ).fetchall()
        return [
            (str(row["task_id"]), str(row["result_sha256"]), bool(row["expected_match"]))
            for row in rows
        ]

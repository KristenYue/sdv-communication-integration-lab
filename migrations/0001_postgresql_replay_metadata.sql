-- Migration contract only. Execution against PostgreSQL is not yet claimed as evidence.
BEGIN;

CREATE TABLE replay_datasets (
    dataset_sha256 char(64) PRIMARY KEY,
    schema_version text NOT NULL,
    task_count integer NOT NULL CHECK (task_count >= 0),
    unique_source_scenarios integer NOT NULL CHECK (unique_source_scenarios >= 0),
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE replay_objects (
    object_sha256 char(64) PRIMARY KEY,
    dataset_sha256 char(64) NOT NULL REFERENCES replay_datasets(dataset_sha256),
    logical_name text NOT NULL,
    object_uri text NOT NULL UNIQUE,
    size_bytes bigint NOT NULL CHECK (size_bytes > 0),
    row_count bigint NOT NULL CHECK (row_count >= 0),
    schema_version text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (dataset_sha256, logical_name)
);

CREATE TYPE replay_job_status AS ENUM ('pending', 'leased', 'completed');

CREATE TABLE replay_jobs (
    dataset_sha256 char(64) NOT NULL REFERENCES replay_datasets(dataset_sha256),
    task_id text NOT NULL,
    status replay_job_status NOT NULL DEFAULT 'pending',
    lease_owner text,
    lease_expires_at timestamptz,
    fencing_token bigint NOT NULL DEFAULT 0 CHECK (fencing_token >= 0),
    attempt integer NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    result_sha256 char(64),
    expected_match boolean,
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (dataset_sha256, task_id),
    CHECK (
        (status = 'pending' AND lease_owner IS NULL AND lease_expires_at IS NULL)
        OR (status = 'leased' AND lease_owner IS NOT NULL AND lease_expires_at IS NOT NULL)
        OR (status = 'completed' AND result_sha256 IS NOT NULL AND expected_match IS NOT NULL)
    )
);

CREATE INDEX replay_jobs_claim_idx
    ON replay_jobs (dataset_sha256, status, lease_expires_at, task_id);

COMMIT;

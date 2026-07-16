-- 0001_init.sql
-- Базовая схема для оболочки: задачи и попытки их выполнения.

CREATE TYPE job_status AS ENUM (
    'queued',
    'processing',
    'done',
    'failed',
    'dead_letter'
);

CREATE TABLE jobs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    idempotency_key TEXT UNIQUE,              -- защита от дублей при повторной отправке клиентом
    client_id       TEXT NOT NULL,
    job_type        TEXT NOT NULL,             -- 'process' | 'validate' | ...
    status          job_status NOT NULL DEFAULT 'queued',
    input_file_ref  TEXT NOT NULL,             -- ссылка на файл в file-storage-service
    result_file_ref TEXT,
    error_message   TEXT,
    max_attempts    INT NOT NULL DEFAULT 3,
    attempt_count   INT NOT NULL DEFAULT 0,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_jobs_status ON jobs (status);
CREATE INDEX idx_jobs_client_id ON jobs (client_id);

-- Отдельная попытка обработки задачи конкретным воркером.
-- Нужна для идемпотентности и диагностики зависших задач (heartbeat).
CREATE TABLE job_attempts (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    job_id          UUID NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
    worker_id       TEXT NOT NULL,
    started_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_heartbeat  TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at     TIMESTAMPTZ,
    succeeded       BOOLEAN,
    error_message   TEXT
);

CREATE INDEX idx_job_attempts_job_id ON job_attempts (job_id);

-- Триггер на обновление updated_at в jobs
CREATE OR REPLACE FUNCTION set_updated_at() RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_jobs_updated_at
    BEFORE UPDATE ON jobs
    FOR EACH ROW
    EXECUTE FUNCTION set_updated_at();

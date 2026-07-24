#include "jobrepository.h"

ExistingJob JobRepository::findByIdempotencyKey(const QString& key)
{
    ExistingJob result;

    QSqlQuery q;

    q.prepare(
        "SELECT id, status "
        "FROM jobs "
        "WHERE idempotency_key=:key");

    q.bindValue(":key", key);

    if (!q.exec())
        return result;

    if (!q.next())
        return result;

    result.found = true;
    result.jobId = q.value("id").toString();
    result.status = q.value("status").toString();

    return result;
}

JobLookup JobRepository::findById(const QString& id)
{
    JobLookup result;

    QSqlQuery q;

    // Тот же SELECT, что раньше был прямо в api-server/main.cpp
    // (обработчик GET /jobs/{id}) — перенесён сюда без изменений.
    q.prepare(
        "SELECT id, client_id, job_type, status, input_file_ref, result_file_ref, "
        "error_message, attempt_count, max_attempts, created_at, updated_at "
        "FROM jobs WHERE id = :id");

    q.bindValue(":id", id);

    if (!q.exec())
    {
        result.queryFailed = true;
        return result;
    }

    if (!q.next())
        return result;

    result.found = true;
    result.fields["id"] = q.value("id");
    result.fields["client_id"] = q.value("client_id");
    result.fields["job_type"] = q.value("job_type");
    result.fields["status"] = q.value("status");
    result.fields["input_file_ref"] = q.value("input_file_ref");
    result.fields["result_file_ref"] = q.value("result_file_ref");
    result.fields["error_message"] = q.value("error_message");
    result.fields["attempt_count"] = q.value("attempt_count");
    result.fields["max_attempts"] = q.value("max_attempts");
    result.fields["created_at"] = q.value("created_at");
    result.fields["updated_at"] = q.value("updated_at");

    return result;
}

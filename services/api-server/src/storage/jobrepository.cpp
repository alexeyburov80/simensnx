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

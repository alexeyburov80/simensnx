#pragma once

#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QSqlError>
#include <QSqlQuery>

struct ExistingJob
{
    bool found = false;

    QString jobId;

    QString status;
};

// findById() раньше возвращал bool + QVariantMap& out-param — этого
// недостаточно, чтобы отличить "задачи с таким id нет" (404) от "запрос к
// БД не выполнился" (500), а вызывающему коду (JobsController::handleGet)
// нужны оба случая. Метод никогда не был реализован и не имел вызывающих
// — поменять контракт сейчас безопасно, ломать нечего.
struct JobLookup
{
    bool found = false;

    bool queryFailed = false;

    QVariantMap fields;
};

class JobRepository
{
public:

    ExistingJob findByIdempotencyKey(const QString& key);

    JobLookup findById(const QString& id);
};

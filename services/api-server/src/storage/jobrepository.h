#pragma once

#include <QString>
#include <QVariant>
#include <QSqlError>
#include <QSqlQuery>

struct ExistingJob
{
    bool found = false;

    QString jobId;

    QString status;
};

class JobRepository
{
public:

    ExistingJob findByIdempotencyKey(const QString& key);

    bool findById(const QString& id, QVariantMap& result);
};

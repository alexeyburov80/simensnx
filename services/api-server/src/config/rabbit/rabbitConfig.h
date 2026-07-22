#pragma once

#include <QString>

struct RabbitConfig
{
    QString processQueue = "jobs.process";

    QString validateQueue = "jobs.validate";

    QString eventsExchange = "jobs.events";

    QString deadLetterExchange = "jobs.dlx";

    QString deadLetterQueue = "jobs.dlq";
};

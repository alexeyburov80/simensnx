#pragma once

#include <QUrl>
#include <QCoreApplication>
#include <QDebug>

#include "rabbit/rabbitConfig.h"

struct AppConfig
{
    QString serviceName = "api-server";

    quint16 port = 8080;

    QUrl rabbitmqUrl;

    QUrl databaseUrl;

    QUrl authServiceUrl;

    RabbitConfig rabbit;
};

AppConfig loadConfig();

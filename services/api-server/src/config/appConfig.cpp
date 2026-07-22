#include "appConfig.h"
#include "env.h"

AppConfig loadConfig()
{
    AppConfig cfg;

    bool ok = false;

    cfg.port = qEnvironmentVariable(Env::Port, "8080").toUShort(&ok);

    if (!ok)
        throw std::runtime_error("Invalid PORT");

    cfg.rabbitmqUrl =
        QUrl(qEnvironmentVariable(
            Env::RabbitmqUrl,
            "amqp://simensnx:simensnx@rabbitmq:5672/"));

    cfg.databaseUrl =
        QUrl(qEnvironmentVariable(
            Env::DatabaseUrl,
            "postgres://simensnx:simensnx@postgres:5432/simensnx"));

    cfg.authServiceUrl =
        QUrl(qEnvironmentVariable(
            Env::AuthServiceUrl,
            "http://auth-stub:8081"));

    return cfg;
}

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QDebug>

#include "GracefulShutdown.h"
#include "HttpServer.h"
#include "JobPublisher.h"
#include "Metrics.h"
#include "PostgresConnection.h"
#include "QtAmqpConnection.h"

#include "auth/AuthStubClient.h"
#include "config/appConfig.h"
#include "jobs/JobsController.h"
#include "storage/jobrepository.h"

namespace {

AppConfig config = loadConfig();

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QString serviceName = "api-server";
    installSignalHandlers(app, serviceName);

    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8080").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[" << serviceName << "] invalid PORT env var";
        return 1;
    }

    if (!config.rabbitmqUrl.isValid() || config.rabbitmqUrl.host().isEmpty()) {
        qCritical() << "[" << serviceName << "] invalid RABBITMQ_URL:" << config.rabbitmqUrl;
        return 1;
    }

    if (!openPostgresConnection(config.databaseUrl)) {
        qCritical() << "[" << serviceName << "] failed to connect to PostgreSQL:"
                     << QSqlDatabase::database().lastError().text();
        return 1;
    }
    qInfo() << "[" << serviceName << "] connected to PostgreSQL at" << config.databaseUrl.host();

    const QUrl authServiceUrl(qEnvironmentVariable("AUTH_SERVICE_URL", "http://auth-stub:8081"));

    QNetworkAccessManager nam;
    QtAmqpConnection amqp;
    QObject::connect(&amqp, &QtAmqpConnection::ready, [&]() {
        qInfo() << "[" << serviceName << "] RabbitMQ channel ready, accepting jobs";
    });
    QObject::connect(&amqp, &QtAmqpConnection::connectionError, [&](const QString &msg) {
        qWarning() << "[" << serviceName << "] RabbitMQ error:" << msg;
    });
    amqp.connectToServer(config.rabbitmqUrl);

    HttpServer server;
    Metrics metrics;

    // Сборка зависимостей для JobsController — вся бизнес-логика POST /jobs
    // и GET /jobs/{id} живёт там (см. jobs/JobsController.h), main() здесь
    // только собирает конкретные реализации и передаёт их через
    // конструктор. amqp реализует IJobPublisher (QtAmqpConnection.h) —
    // единственная причина, по которой JobsController вообще можно
    // тестировать без amqpcpp: он зависит от интерфейса, а не от amqp
    // напрямую.
    AuthStubClient authClient(nam, authServiceUrl);
    JobRepository jobRepository;
    JobsController jobsController(amqp, jobRepository, authClient, metrics, config.rabbit);

    server.addRoute("GET", "/", [](const HttpRequest &) {
        return HttpResponse::json(200, "OK", R"({"status":"ok","service":"api-server"})");
    });

    server.addRoute("GET", "/metrics", [&metrics](const HttpRequest &) {
        HttpResponse resp;
        resp.statusCode = 200;
        resp.statusText = "OK";
        resp.contentType = "text/plain; version=0.0.4";
        resp.body = metrics.render();
        return resp;
    });

    // Health-check раньше всегда отвечал "healthy", даже если соединение с
    // RabbitMQ не было установлено вовсе. Теперь статус реально отражает
    // готовность канала: это то, что должно смотреть readinessProbe.
    server.addRoute("GET", "/health", [&amqp](const HttpRequest &) {
        QSqlQuery q("SELECT 1");
        const bool dbOk = q.isActive();
        if (!dbOk || !amqp.isReady()) {
            QJsonObject obj{
                {"status", "degraded"},
                {"postgres", dbOk ? "ready" : "not_ready"},
                {"rabbitmq", amqp.isReady() ? "ready" : "not_ready"},
            };
            return HttpResponse::json(503, "Service Unavailable", QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }
        return HttpResponse::json(200, "OK", R"({"status":"healthy","postgres":"ready","rabbitmq":"ready"})");
    });

    server.addRoute("POST", "/jobs", [&jobsController](const HttpRequest &req, HttpServer::RespondFn respond) {
        jobsController.handleCreate(req, respond);
    });

    static const QRegularExpression jobIdPattern(R"(^/jobs/([0-9a-fA-F-]{36})$)");
    server.addRoutePattern("GET", jobIdPattern, [&jobsController](const HttpRequest &req) -> HttpResponse {
        return jobsController.handleGet(req);
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[" << serviceName << "] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[" << serviceName << "] listening on port" << port;

    return app.exec();
}

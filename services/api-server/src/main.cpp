#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QUrl>
#include <QUuid>
#include <QDebug>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "HttpServer.h"
#include "QtAmqpConnection.h"

namespace {

// --- Штатное завершение по SIGINT/SIGTERM (self-pipe trick) ---
// В исходном коде "graceful shutdown" стоял ПОСЛЕ блокирующего svr.listen(),
// то есть фактически не выполнялся: контейнер получал SIGKILL по таймауту
// оркестратора. Здесь сигнал реально доводится до Qt event loop.
int g_sigFd[2];

void unixSignalHandler(int) {
    char a = 1;
    if (::write(g_sigFd[1], &a, sizeof(a)) != sizeof(a)) {
        // Сознательно ничего не делаем: мы в обработчике сигнала, куда
        // нельзя safely звать даже qWarning(). Если write не удался,
        // следующий сигнал (SIGKILL от оркестратора после таймаута) всё
        // равно завершит процесс.
    }
}

void installSignalHandlers(QCoreApplication &app) {
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    auto *notifier = new QSocketNotifier(g_sigFd[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app, notifier](QSocketDescriptor, QSocketNotifier::Type) {
        char tmp;
        const auto n = ::read(g_sigFd[0], &tmp, sizeof(tmp));
        (void)n; // значение не важно — сам факт активации notifier'а достаточен
        notifier->setEnabled(false);
        qInfo() << "[api-server] shutdown signal received, stopping";
        app.quit();
    });

    struct sigaction sa{};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

QByteArray errorJson(const QString &message) {
    QJsonObject obj{{"error", message}};
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    const QString serviceName = "api-server";
    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8080").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[" << serviceName << "] invalid PORT env var";
        return 1;
    }

    // Раньше host/user/pass RabbitMQ были захардкожены в коде и полностью
    // игнорировали RABBITMQ_URL, который передаёт docker-compose/Helm.
    // Теперь это единственный источник конфигурации, со значением по
    // умолчанию для локальной разработки.
    const QUrl rabbitmqUrl(qEnvironmentVariable("RABBITMQ_URL", "amqp://simensnx:simensnx@rabbitmq:5672/"));
    if (!rabbitmqUrl.isValid() || rabbitmqUrl.host().isEmpty()) {
        qCritical() << "[" << serviceName << "] invalid RABBITMQ_URL:" << rabbitmqUrl;
        return 1;
    }

    QtAmqpConnection amqp;
    QObject::connect(&amqp, &QtAmqpConnection::ready, [&]() {
        qInfo() << "[" << serviceName << "] RabbitMQ channel ready, accepting jobs";
    });
    QObject::connect(&amqp, &QtAmqpConnection::connectionError, [&](const QString &msg) {
        qWarning() << "[" << serviceName << "] RabbitMQ error:" << msg;
    });
    amqp.connectToServer(rabbitmqUrl);

    HttpServer server;

    server.addRoute("GET", "/", [](const HttpRequest &) {
        return HttpResponse::json(200, "OK", R"({"status":"ok","service":"api-server"})");
    });

    // Health-check раньше всегда отвечал "healthy", даже если соединение с
    // RabbitMQ не было установлено вовсе. Теперь статус реально отражает
    // готовность канала: это то, что должно смотреть readinessProbe.
    server.addRoute("GET", "/health", [&amqp](const HttpRequest &) {
        if (!amqp.isReady()) {
            return HttpResponse::json(503, "Service Unavailable", R"({"status":"degraded","rabbitmq":"not_ready"})");
        }
        return HttpResponse::json(200, "OK", R"({"status":"healthy","rabbitmq":"ready"})");
    });

    server.addRoute("POST", "/jobs", [&amqp](const HttpRequest &req) -> HttpResponse {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(req.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return HttpResponse::json(400, "Bad Request", errorJson("invalid JSON body: " + parseError.errorString()));
        }

        const QJsonObject obj = doc.object();

        // Контракт приведён в соответствие со схемой jobs (db/migrations/0001_init.sql):
        // client_id и input_file_ref там NOT NULL, а поля model_id в схеме
        // вообще нет — использовать его для job-orchestrator было бы нечем.
        const QString clientId = obj.value("client_id").toString();
        const QString jobType = obj.value("job_type").toString();
        const QString inputFileRef = obj.value("input_file_ref").toString();
        const QString idempotencyKey = obj.value("idempotency_key").toString();

        if (clientId.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("client_id is required"));
        }
        if (inputFileRef.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("input_file_ref is required"));
        }
        static const QRegularExpression validTypes("^(process|validate)$");
        if (!validTypes.match(jobType).hasMatch()) {
            return HttpResponse::json(400, "Bad Request", errorJson("job_type must be 'process' or 'validate'"));
        }

        // UUID вместо секундного time(nullptr): у последнего гарантированные
        // коллизии при параллельных запросах в пределах одной секунды, а
        // колонка jobs.id в БД и так UUID.
        const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);

        QJsonObject payload{
            {"job_id", jobId},
            {"client_id", clientId},
            {"job_type", jobType},
            {"input_file_ref", inputFileRef},
        };
        if (!idempotencyKey.isEmpty()) payload["idempotency_key"] = idempotencyKey;

        const QByteArray message = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const QString queue = (jobType == "validate") ? "jobs.validate" : "jobs.process";

        if (!amqp.publish("", queue, message)) {
            // Канал не готов — сообщение НЕ ушло. Раньше publish_job() в
            // этом случае молча ничего не делал, а клиенту всё равно
            // возвращался "status": "pending", как будто всё в порядке.
            return HttpResponse::json(503, "Service Unavailable",
                                       errorJson("RabbitMQ channel is not ready, job was not queued"));
        }

        // Копия того же сообщения — в fanout jobs.events, чтобы
        // job-orchestrator узнал о задаче и сохранил её в PostgreSQL.
        // Не влияет на успешность основного publish выше: если эта
        // публикация не удастся, job всё равно уйдёт в обработку, просто
        // запись в БД появится с опозданием (или после ручной сверки) —
        // это сознательный компромисс Phase 0, где retry-логики ещё нет.
        if (!amqp.publish("jobs.events", "", message)) {
            qWarning() << "[api-server] failed to publish jobs.events copy for job" << jobId;
        }

        QJsonObject respObj{{"job_id", jobId}, {"status", "queued"}, {"queue", queue}};
        return HttpResponse::json(202, "Accepted", QJsonDocument(respObj).toJson(QJsonDocument::Compact));
    });

    static const QRegularExpression jobIdPattern(R"(^/jobs/([0-9a-fA-F-]{36})$)");
    server.addRoutePattern("GET", jobIdPattern, [](const HttpRequest &req) {
        // Настоящий статус живёт в Postgres и им управляет job-orchestrator
        // (см. jobs.status в схеме) — api-server его не хранит и не должен.
        // Пока job-orchestrator не переведён с заглушки на реальную работу
        // с БД, этот эндпоинт честно сообщает об этом, а не выдумывает статус.
        QJsonObject obj{
            {"job_id", req.pathParams.value(0)},
            {"status", "unknown"},
            {"message", "job-orchestrator does not persist job state yet"},
        };
        return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[" << serviceName << "] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[" << serviceName << "] listening on port" << port;

    return app.exec();
}

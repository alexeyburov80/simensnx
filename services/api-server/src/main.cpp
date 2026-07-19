#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
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

bool openDatabase(const QUrl &databaseUrl) {
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL");
    db.setHostName(databaseUrl.host());
    db.setPort(databaseUrl.port(5432));
    QString dbName = databaseUrl.path();
    if (dbName.startsWith('/')) dbName.remove(0, 1);
    db.setDatabaseName(dbName);
    db.setUserName(databaseUrl.userName());
    db.setPassword(databaseUrl.password());
    db.setConnectOptions("connect_timeout=5");
    return db.open();
}

// Единственное место в api-server, которое трогает Postgres — и это
// осознанно только SELECT. Запись в jobs остаётся исключительно за
// job-orchestrator (см. его README) — двух писателей в одну таблицу не
// заводим. Нужен этот SELECT для двух вещей: проверки идемпотентности
// ДО публикации в очередь (см. POST /jobs) и реального GET /jobs/{id}
// вместо прежней заглушки "status: unknown".
struct ExistingJob {
    bool found = false;
    QString jobId;
    QString status;
};

ExistingJob findJobByIdempotencyKey(const QString &idempotencyKey) {
    ExistingJob result;
    QSqlQuery q;
    q.prepare("SELECT id, status FROM jobs WHERE idempotency_key = :key");
    q.bindValue(":key", idempotencyKey);
    if (!q.exec()) {
        qWarning() << "[api-server] idempotency lookup failed:" << q.lastError().text();
        return result; // трактуем как "не найдено" — не блокируем публикацию из-за сбойного SELECT
    }
    if (q.next()) {
        result.found = true;
        result.jobId = q.value("id").toString();
        result.status = q.value("status").toString();
    }
    return result;
}

// Реальный сетевой вызов auth-stub перед публикацией — см. ROADMAP.md,
// Phase 0: "auth-stub: всегда пропускает запрос, но с реальным сетевым
// вызовом (чтобы protocol/latency были видны сразу)". Возвращает false и
// сообщение об ошибке, если auth-stub недоступен или ответил не-200 —
// в этом случае публикация НЕ происходит (fail closed), а не молча
// пропускается, как было бы при отсутствии этой проверки вовсе.
void callAuthStub(QNetworkAccessManager &nam, const QUrl &authServiceUrl, const QByteArray &authHeader,
                   std::function<void(bool allowed, QString error)> callback) {
    QUrl url = authServiceUrl;
    url.setPath(url.path() + "/verify");

    QNetworkRequest request(url);
    if (!authHeader.isEmpty()) {
        request.setRawHeader("Authorization", authHeader);
    }

    auto *reply = nam.post(request, QByteArray());
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(3000);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            reply->deleteLater();
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!doc.isObject() || !doc.object().value("allowed").toBool(false)) {
            callback(false, "auth-stub denied the request");
            return;
        }
        callback(true, {});
    });
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

    const QUrl databaseUrl(qEnvironmentVariable("DATABASE_URL", "postgres://simensnx:simensnx@postgres:5432/simensnx"));
    if (!openDatabase(databaseUrl)) {
        qCritical() << "[" << serviceName << "] failed to connect to PostgreSQL:"
                     << QSqlDatabase::database().lastError().text();
        return 1;
    }
    qInfo() << "[" << serviceName << "] connected to PostgreSQL at" << databaseUrl.host();

    const QUrl authServiceUrl(qEnvironmentVariable("AUTH_SERVICE_URL", "http://auth-stub:8081"));

    QNetworkAccessManager nam;
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

    server.addRoute("POST", "/jobs", [&amqp, &nam, authServiceUrl](const HttpRequest &req, HttpServer::RespondFn respond) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(req.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            respond(HttpResponse::json(400, "Bad Request", errorJson("invalid JSON body: " + parseError.errorString())));
            return;
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
            respond(HttpResponse::json(400, "Bad Request", errorJson("client_id is required")));
            return;
        }
        if (inputFileRef.isEmpty()) {
            respond(HttpResponse::json(400, "Bad Request", errorJson("input_file_ref is required")));
            return;
        }
        static const QRegularExpression validTypes("^(process|validate)$");
        if (!validTypes.match(jobType).hasMatch()) {
            respond(HttpResponse::json(400, "Bad Request", errorJson("job_type must be 'process' or 'validate'")));
            return;
        }

        const QByteArray authHeader = req.headers.value("authorization");

        callAuthStub(nam, authServiceUrl, authHeader, [&amqp, respond, clientId, jobType, inputFileRef, idempotencyKey](bool allowed, QString authError) {
            if (!allowed) {
                // auth-stub недоступен или отказал — fail closed, а не
                // тихо публикуем без проверки (это свело бы на нет весь
                // смысл её вызова).
                qWarning() << "[api-server] auth check failed:" << authError;
                respond(HttpResponse::json(503, "Service Unavailable",
                                            errorJson("auth service unavailable: " + authError)));
                return;
            }

            // Идемпотентность ДО публикации, а не после (как было раньше,
            // когда job-orchestrator ловил дубль постфактум в БД, а в
            // очередь уже улетали два разных job_id на одну и ту же
            // клиентскую отправку). Теперь при повторе с тем же
            // idempotency_key ничего заново не публикуется — клиенту
            // возвращается тот же job_id, что и в первый раз.
            if (!idempotencyKey.isEmpty()) {
                const ExistingJob existing = findJobByIdempotencyKey(idempotencyKey);
                if (existing.found) {
                    QJsonObject respObj{
                        {"job_id", existing.jobId},
                        {"status", existing.status},
                        {"idempotent_replay", true},
                    };
                    respond(HttpResponse::json(200, "OK", QJsonDocument(respObj).toJson(QJsonDocument::Compact)));
                    return;
                }
            }

            // UUID вместо секундного time(nullptr): у последнего
            // гарантированные коллизии при параллельных запросах в
            // пределах одной секунды, а колонка jobs.id в БД и так UUID.
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
                // Канал не готов — сообщение НЕ ушло. Раньше publish_job()
                // в этом случае молча ничего не делал, а клиенту всё равно
                // возвращался "status": "pending", как будто всё в порядке.
                respond(HttpResponse::json(503, "Service Unavailable",
                                            errorJson("RabbitMQ channel is not ready, job was not queued")));
                return;
            }

            // Копия того же сообщения — в fanout jobs.events, чтобы
            // job-orchestrator узнал о задаче и сохранил её в PostgreSQL.
            // Не влияет на успешность основного publish выше: если эта
            // публикация не удастся, job всё равно уйдёт в обработку,
            // просто запись в БД появится с опозданием (или после ручной
            // сверки) — это сознательный компромисс Phase 0, где
            // retry-логики ещё нет.
            if (!amqp.publish("jobs.events", "", message)) {
                qWarning() << "[api-server] failed to publish jobs.events copy for job" << jobId;
            }

            QJsonObject respObj{{"job_id", jobId}, {"status", "queued"}, {"queue", queue}};
            respond(HttpResponse::json(202, "Accepted", QJsonDocument(respObj).toJson(QJsonDocument::Compact)));
        });
    });

    static const QRegularExpression jobIdPattern(R"(^/jobs/([0-9a-fA-F-]{36})$)");
    server.addRoutePattern("GET", jobIdPattern, [](const HttpRequest &req) -> HttpResponse {
        // Реальное чтение из Postgres — раньше здесь всегда возвращался
        // фиктивный "status": "unknown". job-orchestrator теперь пишет
        // состояние по-настоящему (см. его README), так что читать можно.
        QSqlQuery q;
        q.prepare("SELECT id, client_id, job_type, status, input_file_ref, result_file_ref, "
                  "error_message, attempt_count, max_attempts, created_at, updated_at "
                  "FROM jobs WHERE id = :id");
        q.bindValue(":id", req.pathParams.value(0));
        if (!q.exec()) {
            qWarning() << "[api-server] job lookup failed:" << q.lastError().text();
            return HttpResponse::json(500, "Internal Server Error", errorJson("database query failed"));
        }
        if (!q.next()) {
            return HttpResponse::json(404, "Not Found", errorJson("job not found"));
        }

        QJsonObject obj;
        obj["job_id"] = q.value("id").toString();
        obj["client_id"] = q.value("client_id").toString();
        obj["job_type"] = q.value("job_type").toString();
        obj["status"] = q.value("status").toString();
        obj["input_file_ref"] = q.value("input_file_ref").toString();
        const QVariant resultRef = q.value("result_file_ref");
        if (!resultRef.isNull()) obj["result_file_ref"] = resultRef.toString();
        const QVariant errMsg = q.value("error_message");
        if (!errMsg.isNull()) obj["error_message"] = errMsg.toString();
        obj["attempt_count"] = q.value("attempt_count").toInt();
        obj["max_attempts"] = q.value("max_attempts").toInt();
        obj["created_at"] = q.value("created_at").toDateTime().toUTC().toString(Qt::ISODate);
        obj["updated_at"] = q.value("updated_at").toDateTime().toUTC().toString(Qt::ISODate);
        return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[" << serviceName << "] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[" << serviceName << "] listening on port" << port;

    return app.exec();
}

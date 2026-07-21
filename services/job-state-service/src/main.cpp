#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

#include <algorithm>
#include <csignal>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#include "HttpServer.h"
#include "Metrics.h"
#include "QtAmqpConsumer.h"

namespace {

int g_sigFd[2];

void unixSignalHandler(int) {
    char a = 1;
    if (::write(g_sigFd[1], &a, sizeof(a)) != sizeof(a)) {
    }
}

void installSignalHandlers(QCoreApplication &app) {
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    auto *notifier = new QSocketNotifier(g_sigFd[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app, notifier](QSocketDescriptor, QSocketNotifier::Type) {
        char tmp;
        const auto n = ::read(g_sigFd[0], &tmp, sizeof(tmp));
        (void)n;
        notifier->setEnabled(false);
        qInfo() << "[job-state-service] shutdown signal received, stopping";
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
    return QJsonDocument(QJsonObject{{"error", message}}).toJson(QJsonDocument::Compact);
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

QJsonObject jobRowToJson(const QSqlQuery &q) {
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
    return obj;
}

// Повторно ставит задачу в работу или переводит её в терминальный
// dead_letter — единая точка принятия решения "ретраить или сдаться",
// используется и для событий status=failed от воркера, и для sweep'а
// зависших в processing задач (см. main()). Это и есть тот самый
// retry-цикл из ROADMAP.md Phase 1 ("job-state-service: retry, обработка
// таймаутов") — раньше здесь была только защита от бесконечного цикла на
// баге с гонкой, а не бизнес-retry.
void retryOrDeadLetter(QtAmqpConsumer &amqp, Metrics &metrics, const QString &jobId, const QString &errorMessage) {
    QSqlQuery q;
    q.prepare("SELECT client_id, job_type, input_file_ref, attempt_count, max_attempts "
              "FROM jobs WHERE id = :id");
    q.bindValue(":id", jobId);
    if (!q.exec() || !q.next()) {
        qWarning() << "[job-state-service] retryOrDeadLetter: job" << jobId << "not found, cannot retry";
        return;
    }

    const QString clientId = q.value("client_id").toString();
    const QString jobType = q.value("job_type").toString();
    const QString inputFileRef = q.value("input_file_ref").toString();
    const int attemptCount = q.value("attempt_count").toInt();
    const int maxAttempts = q.value("max_attempts").toInt();

    if (attemptCount < maxAttempts) {
        // attempt_count уже учтён в момент, когда воркер прислал
        // "processing" (см. services/nx-worker-stub) — здесь его заново не
        // увеличиваем, следующая попытка сама себя посчитает, когда
        // какой-нибудь воркер снова возьмёт задачу в работу.
        const QString queue = (jobType == "validate") ? "jobs.validate" : "jobs.process";
        const QJsonObject payload{
            {"job_id", jobId}, {"client_id", clientId}, {"job_type", jobType}, {"input_file_ref", inputFileRef},
        };
        const bool published = amqp.publish("", queue, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        if (!published) {
            qWarning() << "[job-state-service] failed to republish job" << jobId << "for retry - AMQP channel not ready,"
                       << "leaving status as-is, will be retried by the next sweep";
            return;
        }

        QSqlQuery upd;
        upd.prepare("UPDATE jobs SET status = 'queued', error_message = :err WHERE id = :id");
        upd.bindValue(":err", errorMessage);
        upd.bindValue(":id", jobId);
        upd.exec();
        qInfo() << "[job-state-service] job" << jobId << "requeued for retry (" << (attemptCount + 1) << "/" << maxAttempts
                << "attempts used), queue=" << queue;
        metrics.inc("job_state_jobs_retried_total", "Total jobs requeued for another attempt after failure",
                    {{"job_type", jobType}});
    } else {
        QSqlQuery upd;
        upd.prepare("UPDATE jobs SET status = 'dead_letter', error_message = :err WHERE id = :id");
        upd.bindValue(":err", errorMessage);
        upd.bindValue(":id", jobId);
        upd.exec();
        qWarning() << "[job-state-service] job" << jobId << "exhausted" << maxAttempts
                   << "attempts - moved to dead_letter:" << errorMessage;
        metrics.inc("job_state_jobs_dead_lettered_total", "Total jobs moved to dead_letter after exhausting max_attempts",
                    {{"job_type", jobType}});
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8082").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[job-state-service] invalid PORT env var";
        return 1;
    }

    const QUrl databaseUrl(qEnvironmentVariable("DATABASE_URL",
        "postgres://simensnx:simensnx@postgres:5432/simensnx"));
    if (!openDatabase(databaseUrl)) {
        qCritical() << "[job-state-service] failed to connect to PostgreSQL:"
                     << QSqlDatabase::database().lastError().text();
        return 1;
    }
    qInfo() << "[job-state-service] connected to PostgreSQL at" << databaseUrl.host()
            << "db=" << QSqlDatabase::database().databaseName()
            << "pid=" << QCoreApplication::applicationPid();

    const QUrl rabbitmqUrl(qEnvironmentVariable("RABBITMQ_URL", "amqp://simensnx:simensnx@rabbitmq:5672/"));
    if (!rabbitmqUrl.isValid() || rabbitmqUrl.host().isEmpty()) {
        qCritical() << "[job-state-service] invalid RABBITMQ_URL:" << rabbitmqUrl;
        return 1;
    }

    bool stuckOk = false;
    const int stuckTimeoutSeconds = qEnvironmentVariable("JOB_STUCK_TIMEOUT_SECONDS", "120").toInt(&stuckOk);

    QtAmqpConsumer amqp;
    Metrics metrics;
    QObject::connect(&amqp, &QtAmqpConsumer::connectionError, [](const QString &msg) {
        qWarning() << "[job-state-service] RabbitMQ error:" << msg;
    });

    // Счётчик попыток применить статусное событие к job_id, которого ещё
    // (или уже никогда) нет в БД. Без этой защиты сообщение, для которого
    // intake в принципе никогда не придёт (например, потому что api-server
    // работает на образе без публикации в jobs.events), requeue'ится
    // мгновенно и бесконечно, забивая CPU и логи. Живёт в памяти процесса и
    // сбрасывается при рестарте — это осознанный компромисс, отдельный от
    // retryOrDeadLetter() выше (тот — про бизнес-retry обработки задачи,
    // этот — про защиту от собственного бага рассинхронизации событий).
    auto unknownJobRetries = std::make_shared<QHash<QString, int>>();
    constexpr int kMaxUnknownJobRetries = 8;

    QObject::connect(&amqp, &QtAmqpConsumer::ready, [&amqp, &metrics, unknownJobRetries]() {
        qInfo() << "[job-state-service] RabbitMQ channel ready, starting consumers";

        // jobs.process/jobs.validate объявляются здесь ТЕМИ ЖЕ аргументами,
        // что и в api-server/nx-worker-stub (durable, без Table-аргументов) —
        // нужны для republish при retry, см. retryOrDeadLetter().
        amqp.declareWorkQueues();

        // Consume копии события о создании задачи из fanout-обменника jobs.events
        // (объявляет и публикует его api-server, см. services/api-server).
        amqp.consumeFromFanoutExchange("jobs.events", "jobs.job-state-service.intake", 10,
            [&amqp, &metrics](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
                const QByteArray body(message.body(), static_cast<int>(message.bodySize()));

                QJsonParseError parseError{};
                const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    qWarning() << "[job-state-service] malformed jobs.events message, rejecting without requeue:"
                               << parseError.errorString();
                    amqp.reject(deliveryTag, false);
                    return;
                }

                const QJsonObject obj = doc.object();
                const QString jobId = obj.value("job_id").toString();
                const QString clientId = obj.value("client_id").toString();
                const QString jobType = obj.value("job_type").toString();
                const QString inputFileRef = obj.value("input_file_ref").toString();
                const QString idempotencyKey = obj.value("idempotency_key").toString();

                if (jobId.isEmpty() || clientId.isEmpty() || jobType.isEmpty() || inputFileRef.isEmpty()) {
                    qWarning() << "[job-state-service] jobs.events message missing required fields, rejecting:"
                               << QString::fromUtf8(body);
                    amqp.reject(deliveryTag, false);
                    return;
                }

                QSqlQuery q;
                bool prepared;
                if (idempotencyKey.isEmpty()) {
                    prepared = q.prepare(
                        "INSERT INTO jobs (id, client_id, job_type, status, input_file_ref) "
                        "VALUES (:id, :client_id, :job_type, 'queued', :input_file_ref) "
                        "ON CONFLICT (id) DO NOTHING");
                } else {
                    prepared = q.prepare(
                        "INSERT INTO jobs (id, client_id, job_type, status, input_file_ref, idempotency_key) "
                        "VALUES (:id, :client_id, :job_type, 'queued', :input_file_ref, :idempotency_key) "
                        "ON CONFLICT (idempotency_key) DO NOTHING");
                    q.bindValue(":idempotency_key", idempotencyKey);
                }
                q.bindValue(":id", jobId);
                q.bindValue(":client_id", clientId);
                q.bindValue(":job_type", jobType);
                q.bindValue(":input_file_ref", inputFileRef);

                if (!prepared || !q.exec()) {
                    qWarning() << "[job-state-service] failed to persist job" << jobId << ":" << q.lastError().text();
                    return;
                }

                qInfo() << "[job-state-service] persisted job" << jobId << "(redelivered=" << redelivered << ")"
                        << "pid=" << QCoreApplication::applicationPid();
                metrics.inc("job_state_jobs_persisted_total", "Total jobs persisted to PostgreSQL from jobs.events",
                            {{"job_type", jobType}});
                amqp.ack(deliveryTag);
            });

        // Статусные события от nx-worker-stub: processing -> done | failed.
        // failed теперь не терминальное состояние само по себе —
        // retryOrDeadLetter() решает, вернуть задачу в очередь или отправить
        // в dead_letter, в зависимости от attempt_count/max_attempts.
        amqp.consumeFromFanoutExchange("jobs.status", "jobs.job-state-service.status", 10,
            [&amqp, &metrics, unknownJobRetries](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
                const QByteArray body(message.body(), static_cast<int>(message.bodySize()));

                QJsonParseError parseError{};
                const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    qWarning() << "[job-state-service] malformed jobs.status message, rejecting:"
                               << parseError.errorString();
                    amqp.reject(deliveryTag, false);
                    return;
                }

                const QJsonObject obj = doc.object();
                const QString jobId = obj.value("job_id").toString();
                const QString status = obj.value("status").toString();
                const QString resultFileRef = obj.value("result_file_ref").toString();
                const QString errorMessage = obj.value("error_message").toString();

                if (jobId.isEmpty() || status.isEmpty()) {
                    qWarning() << "[job-state-service] jobs.status message missing job_id/status, rejecting:"
                               << QString::fromUtf8(body);
                    amqp.reject(deliveryTag, false);
                    return;
                }

                metrics.inc("job_state_status_events_total", "Total jobs.status events received", {{"status", status}});

                if (status == "failed") {
                    // retryOrDeadLetter сама делает SELECT+UPDATE (и, если
                    // нужно, republish) — здесь просто отдаём ей управление
                    // и подтверждаем приём события. numRowsAffected здесь не
                    // проверяем: если строки нет, retryOrDeadLetter молча
                    // логирует и ничего не делает (см. её реализацию) — race
                    // с intake для failed-события маловероятна на практике
                    // (между "processing" и "failed" всегда проходит время
                    // на реальную работу), поэтому отдельный backoff-цикл
                    // здесь не заводили, в отличие от processing/done ниже.
                    retryOrDeadLetter(amqp, metrics, jobId, errorMessage);
                    amqp.ack(deliveryTag);
                    return;
                }

                QSqlQuery q;
                if (status == "processing") {
                    q.prepare("UPDATE jobs SET status = 'processing', attempt_count = attempt_count + 1 "
                              "WHERE id = :id");
                    q.bindValue(":id", jobId);
                } else {
                    q.prepare(
                        "UPDATE jobs SET status = :status, "
                        "result_file_ref = COALESCE(NULLIF(:result_file_ref, ''), result_file_ref), "
                        "error_message = COALESCE(NULLIF(:error_message, ''), error_message) "
                        "WHERE id = :id");
                    q.bindValue(":status", status);
                    q.bindValue(":result_file_ref", resultFileRef);
                    q.bindValue(":error_message", errorMessage);
                    q.bindValue(":id", jobId);
                }

                if (!q.exec()) {
                    // enum job_status отвергнет неизвестный статус здесь же —
                    // и это безопасно: сообщение останется unacked и не
                    // потеряется, но и мусор в БД не попадёт.
                    qWarning() << "[job-state-service] failed to apply status" << status << "for job" << jobId << ":"
                               << q.lastError().text();
                    return;
                }
                if (q.numRowsAffected() == 0) {
                    // См. подробный комментарий про это в истории отладки —
                    // либо настоящая гонка intake/status для одного job_id
                    // (штатно разрешается за 1-2 попытки), либо постоянная
                    // рассинхронизация (например, старая версия api-server) —
                    // тогда лимит попыток не даст уйти в busy-loop.
                    const int attempts = ++(*unknownJobRetries)[jobId];
                    if (attempts > kMaxUnknownJobRetries) {
                        qCritical() << "[job-state-service] job" << jobId << "still unknown after" << attempts
                                    << "attempts - giving up on this status event (status=" << status << ").";
                        unknownJobRetries->remove(jobId);
                        amqp.reject(deliveryTag, false);
                        return;
                    }
                    const int delayMs = std::min(500 * (1 << attempts), 15000);
                    qWarning() << "[job-state-service] status" << status << "for unknown job" << jobId
                               << "- attempt" << attempts << "of" << kMaxUnknownJobRetries
                               << ", retrying in" << delayMs << "ms";
                    QTimer::singleShot(delayMs, [&amqp, deliveryTag]() { amqp.reject(deliveryTag, true); });
                    return;
                }

                unknownJobRetries->remove(jobId);
                qInfo() << "[job-state-service] job" << jobId << "-> status" << status
                        << "(redelivered=" << redelivered << ")";
                amqp.ack(deliveryTag);
            });
    });
    amqp.connectToServer(rabbitmqUrl);

    // Sweep зависших в processing задач — воркер мог упасть посреди работы,
    // так и не прислав ни done, ни failed. Без этого такая задача осталась
    // бы в processing навсегда, а клиент никогда не узнал бы, что она не
    // выполнится. attempt_count уже увеличен в момент входа в processing,
    // поэтому здесь просто применяем то же решение retry/dead_letter, что и
    // для явного failed от воркера.
    QTimer stuckSweepTimer;
    QObject::connect(&stuckSweepTimer, &QTimer::timeout, [&amqp, &metrics, stuckTimeoutSeconds]() {
        QSqlQuery q;
        q.prepare("SELECT id FROM jobs WHERE status = 'processing' "
                  "AND updated_at < now() - (:timeout_seconds || ' seconds')::interval");
        q.bindValue(":timeout_seconds", stuckTimeoutSeconds);
        if (!q.exec()) {
            qWarning() << "[job-state-service] stuck-job sweep query failed:" << q.lastError().text();
            return;
        }
        QStringList stuckIds;
        while (q.next()) stuckIds << q.value("id").toString();
        for (const QString &jobId : stuckIds) {
            qWarning() << "[job-state-service] job" << jobId << "stuck in processing for over"
                       << stuckTimeoutSeconds << "s - treating as failed attempt";
            metrics.inc("job_state_stuck_jobs_total", "Total jobs found stuck in processing by the periodic sweep");
            retryOrDeadLetter(amqp, metrics, jobId, "worker did not report completion within timeout");
        }
    });
    stuckSweepTimer.start(30000); // проверяем раз в 30с

    HttpServer server;

    server.addRoute("GET", "/metrics", [&metrics](const HttpRequest &) {
        HttpResponse resp;
        resp.statusCode = 200;
        resp.statusText = "OK";
        resp.contentType = "text/plain; version=0.0.4";
        resp.body = metrics.render();
        return resp;
    });

    server.addRoute("GET", "/health", [&amqp](const HttpRequest &) {
        QSqlQuery q("SELECT 1"); // конструктор с текстом запроса выполняет его сразу
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

    static const QRegularExpression jobIdPattern(R"(^/jobs/([0-9a-fA-F-]{36})$)");
    server.addRoutePattern("GET", jobIdPattern, [](const HttpRequest &req) -> HttpResponse {
        QSqlQuery q;
        q.prepare("SELECT id, client_id, job_type, status, input_file_ref, result_file_ref, "
                  "error_message, attempt_count, max_attempts, created_at, updated_at "
                  "FROM jobs WHERE id = :id");
        q.bindValue(":id", req.pathParams.value(0));
        if (!q.exec()) {
            qWarning() << "[job-state-service] query failed:" << q.lastError().text();
            return HttpResponse::json(500, "Internal Server Error", errorJson("database query failed"));
        }
        if (!q.next()) {
            return HttpResponse::json(404, "Not Found", errorJson("job not found"));
        }
        return HttpResponse::json(200, "OK", QJsonDocument(jobRowToJson(q)).toJson(QJsonDocument::Compact));
    });

    server.addRoute("GET", "/jobs", [](const HttpRequest &) {
        QSqlQuery q("SELECT id, client_id, job_type, status, input_file_ref, result_file_ref, "
                    "error_message, attempt_count, max_attempts, created_at, updated_at "
                    "FROM jobs ORDER BY created_at DESC LIMIT 50");
        QJsonArray items;
        while (q.next()) items.append(jobRowToJson(q));
        return HttpResponse::json(200, "OK", QJsonDocument(items).toJson(QJsonDocument::Compact));
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[job-state-service] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[job-state-service] listening on port" << port << "stuck-job timeout=" << stuckTimeoutSeconds << "s";

    return app.exec();
}

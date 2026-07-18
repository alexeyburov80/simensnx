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
        qInfo() << "[job-orchestrator] shutdown signal received, stopping";
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

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8082").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[job-orchestrator] invalid PORT env var";
        return 1;
    }

    const QUrl databaseUrl(qEnvironmentVariable("DATABASE_URL",
        "postgres://simensnx:simensnx@postgres:5432/simensnx"));
    if (!openDatabase(databaseUrl)) {
        qCritical() << "[job-orchestrator] failed to connect to PostgreSQL:"
                     << QSqlDatabase::database().lastError().text();
        return 1;
    }
    qInfo() << "[job-orchestrator] connected to PostgreSQL at" << databaseUrl.host()
            << "db=" << QSqlDatabase::database().databaseName()
            << "pid=" << QCoreApplication::applicationPid();

    const QUrl rabbitmqUrl(qEnvironmentVariable("RABBITMQ_URL", "amqp://simensnx:simensnx@rabbitmq:5672/"));
    if (!rabbitmqUrl.isValid() || rabbitmqUrl.host().isEmpty()) {
        qCritical() << "[job-orchestrator] invalid RABBITMQ_URL:" << rabbitmqUrl;
        return 1;
    }

    QtAmqpConsumer amqp;
    QObject::connect(&amqp, &QtAmqpConsumer::connectionError, [](const QString &msg) {
        qWarning() << "[job-orchestrator] RabbitMQ error:" << msg;
    });

    // Счётчик попыток применить jobs.completed к job_id, которого ещё (или
    // уже никогда) нет в БД. Без этой защиты сообщение, для которого intake
    // в принципе никогда не придёт (например, потому что api-server не
    // публикует jobs.events — см. диагностику ниже), requeue'ится мгновенно
    // и бесконечно, забивая CPU и логи. Живёт в памяти процесса и
    // сбрасывается при рестарте — это осознанный компромисс Phase 0, а не
    // полноценный retry с персистентным backoff.
    auto unknownJobRetries = std::make_shared<QHash<QString, int>>();
    constexpr int kMaxUnknownJobRetries = 8;

    // Consume копии события о создании задачи из fanout-обменника jobs.events
    // (объявляет и публикует его api-server, см. services/api-server).
    // jobs.process/jobs.validate, которые реально забирает nx-worker-stub,
    // здесь не трогаем — у orchestrator своя отдельная очередь-копия.
    QObject::connect(&amqp, &QtAmqpConsumer::ready, [&amqp, unknownJobRetries]() {
        qInfo() << "[job-orchestrator] RabbitMQ channel ready, starting intake consumer";
        amqp.consumeFromFanoutExchange("jobs.events", "jobs.orchestrator.intake", 10,
            [&amqp](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
                const QByteArray body(message.body(), static_cast<int>(message.bodySize()));

                QJsonParseError parseError{};
                const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    qWarning() << "[job-orchestrator] malformed jobs.events message, rejecting without requeue:"
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
                    qWarning() << "[job-orchestrator] jobs.events message missing required fields, rejecting:"
                               << QString::fromUtf8(body);
                    amqp.reject(deliveryTag, false);
                    return;
                }

                QSqlQuery q;
                bool prepared;
                if (idempotencyKey.isEmpty()) {
                    // Без idempotency_key единственная защита от дублей —
                    // сам id (UUID, сгенерированный один раз в api-server);
                    // конфликт здесь означает redelivery того же сообщения.
                    prepared = q.prepare(
                        "INSERT INTO jobs (id, client_id, job_type, status, input_file_ref) "
                        "VALUES (:id, :client_id, :job_type, 'queued', :input_file_ref) "
                        "ON CONFLICT (id) DO NOTHING");
                } else {
                    // С idempotency_key именно он — источник истины о
                    // дубле повторной отправки клиента, а не id.
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
                    // Реальная ошибка БД (не дубль, а, например, обрыв
                    // соединения) — НЕ ack и НЕ reject: сообщение остаётся
                    // unacked и будет доставлено повторно, когда канал
                    // переоткроется (или после restart процесса).
                    qWarning() << "[job-orchestrator] failed to persist job" << jobId << ":" << q.lastError().text();
                    return;
                }

                qInfo() << "[job-orchestrator] persisted job" << jobId << "(redelivered=" << redelivered << ")"
                        << "pid=" << QCoreApplication::applicationPid();
                amqp.ack(deliveryTag);
            });

        // События завершения от nx-worker-stub — единственное место в
        // системе, где jobs.status меняется с 'queued' на что-то другое.
        // Валидные статусы ограничены enum'ом job_status в БД (см.
        // db/migrations/0001_init.sql) — если воркер вдруг пришлёт что-то
        // иное, INSERT/UPDATE упадёт на уровне Postgres, а не тихо запишет
        // мусор.
        amqp.consumeFromFanoutExchange("jobs.completed", "jobs.orchestrator.completions", 10,
            [&amqp, unknownJobRetries](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
                const QByteArray body(message.body(), static_cast<int>(message.bodySize()));

                QJsonParseError parseError{};
                const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    qWarning() << "[job-orchestrator] malformed jobs.completed message, rejecting:"
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
                    qWarning() << "[job-orchestrator] jobs.completed message missing job_id/status, rejecting:"
                               << QString::fromUtf8(body);
                    amqp.reject(deliveryTag, false);
                    return;
                }

                QSqlQuery q;
                q.prepare(
                    "UPDATE jobs SET status = :status, "
                    "result_file_ref = COALESCE(NULLIF(:result_file_ref, ''), result_file_ref), "
                    "error_message = COALESCE(NULLIF(:error_message, ''), error_message), "
                    "attempt_count = attempt_count + 1 "
                    "WHERE id = :id");
                q.bindValue(":status", status);
                q.bindValue(":result_file_ref", resultFileRef);
                q.bindValue(":error_message", errorMessage);
                q.bindValue(":id", jobId);

                if (!q.exec()) {
                    // enum job_status отвергнет неизвестный статус здесь же —
                    // и это abezopasno: сообщение останется unacked и не
                    // потеряется, но и мусор в БД не попадёт.
                    qWarning() << "[job-orchestrator] failed to apply completion for job" << jobId << ":"
                               << q.lastError().text();
                    return;
                }
                if (q.numRowsAffected() == 0) {
                    // Диагностика: явным SELECT проверяем, есть ли строка вообще
                    // и с каким статусом её видит ЭТОТ инстанс job-orchestrator.
                    // Если тут "exists=false" и одновременно где-то в логах есть
                    // "persisted job" с ДРУГИМ pid — это два конкурирующих
                    // процесса на одних очередях (например, не остановленный
                    // старый контейнер), а не гонка внутри одного процесса.
                    QSqlQuery check;
                    check.prepare("SELECT status FROM jobs WHERE id = :id");
                    check.bindValue(":id", jobId);
                    const bool rowExists = check.exec() && check.next();
                    qWarning() << "[job-orchestrator] diagnostic: row for" << jobId
                               << "exists=" << rowExists
                               << (rowExists ? ("current_status=" + check.value("status").toString()) : QString())
                               << "seen by pid=" << QCoreApplication::applicationPid();

                    // Может быть настоящей гонкой (jobs.completed пришёл
                    // раньше своего jobs.events для того же job_id — два
                    // независимых consumer'а на одном канале, порядок между
                    // ними не гарантирован), а может быть постоянным
                    // состоянием: intake для этого job_id не придёт никогда
                    // (типичная причина — api-server работает на образе без
                    // публикации в jobs.events). С одной попытки это не
                    // различить, поэтому: несколько попыток с нарастающей
                    // паузой, а затем — громкий отказ вместо бесконечного
                    // busy-loop (именно это было при мгновенном requeue=true
                    // без лимита — сообщение возвращается в очередь и тут же
                    // редоставляется тому же consumer'у, ничего не ожидая).
                    const int attempts = ++(*unknownJobRetries)[jobId];
                    if (attempts > kMaxUnknownJobRetries) {
                        qCritical() << "[job-orchestrator] job" << jobId << "still unknown after" << attempts
                                    << "attempts - giving up on this completion event. Most likely cause:"
                                    << "api-server is not publishing to the jobs.events exchange (check it was"
                                    << "rebuilt/redeployed with the patch that adds this), or the job_id in the"
                                    << "message doesn't match any row job-orchestrator ever inserted.";
                        unknownJobRetries->remove(jobId);
                        amqp.reject(deliveryTag, false); // без requeue — иначе снова бесконечный цикл
                        return;
                    }

                    const int delayMs = std::min(500 * (1 << attempts), 15000); // ~1с, 2с, 4с ... до 15с
                    qWarning() << "[job-orchestrator] completion for unknown job" << jobId
                               << "- attempt" << attempts << "of" << kMaxUnknownJobRetries
                               << ", retrying in" << delayMs << "ms";
                    QTimer::singleShot(delayMs, [&amqp, deliveryTag]() { amqp.reject(deliveryTag, true); });
                    return;
                }

                unknownJobRetries->remove(jobId);
                qInfo() << "[job-orchestrator] job" << jobId << "-> status" << status
                        << "(redelivered=" << redelivered << ")";
                amqp.ack(deliveryTag);
            });
    });
    amqp.connectToServer(rabbitmqUrl);

    HttpServer server;

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
            qWarning() << "[job-orchestrator] query failed:" << q.lastError().text();
            return HttpResponse::json(500, "Internal Server Error", errorJson("database query failed"));
        }
        if (!q.next()) {
            return HttpResponse::json(404, "Not Found", errorJson("job not found"));
        }
        return HttpResponse::json(200, "OK", QJsonDocument(jobRowToJson(q)).toJson(QJsonDocument::Compact));
    });

    server.addRoute("GET", "/jobs", [](const HttpRequest &) {
        // Базовый листинг без пагинации по курсору — намеренно просто для
        // Phase 0 ("пишет и читает состояние, реального retry-цикла нет").
        QSqlQuery q("SELECT id, client_id, job_type, status, input_file_ref, result_file_ref, "
                    "error_message, attempt_count, max_attempts, created_at, updated_at "
                    "FROM jobs ORDER BY created_at DESC LIMIT 50");
        QJsonArray items;
        while (q.next()) items.append(jobRowToJson(q));
        return HttpResponse::json(200, "OK", QJsonDocument(items).toJson(QJsonDocument::Compact));
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[job-orchestrator] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[job-orchestrator] listening on port" << port;

    return app.exec();
}

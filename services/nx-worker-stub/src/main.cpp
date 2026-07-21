#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSocketNotifier>
#include <QTimer>
#include <QUrl>
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

// --- graceful shutdown (тот же self-pipe trick, что в api-server) ---
int g_sigFd[2];

void unixSignalHandler(int) {
    char a = 1;
    if (::write(g_sigFd[1], &a, sizeof(a)) != sizeof(a)) {
        // см. комментарий в api-server/src/main.cpp — в обработчике сигнала
        // безопасно писать в самопайп и больше ничего.
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
        qInfo() << "[nx-worker-stub] shutdown signal received, stopping";
        app.quit();
    });

    struct sigaction sa{};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Дергает GET {baseUrl}/health с таймаутом и зовёт callback(true/false).
// file-storage-service на момент написания этого сервиса ЕЩЁ САМ являлся
// пустой заглушкой — поэтому здесь ожидаемо было connection refused, и это
// не баг воркера. Теперь file-storage-service отвечает по-настоящему.
void checkDependency(QNetworkAccessManager &nam, const QUrl &baseUrl, const QString &label,
                      std::function<void(bool)> callback) {
    QUrl url = baseUrl;
    url.setPath(url.path() + "/health");

    auto *reply = nam.get(QNetworkRequest(url));
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(3000);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, label, callback]() {
        const bool ok = reply->error() == QNetworkReply::NoError;
        if (!ok) {
            qWarning() << "[nx-worker-stub] dependency check failed:" << label << "->" << reply->errorString();
        } else {
            qInfo() << "[nx-worker-stub] dependency check ok:" << label;
        }
        reply->deleteLater();
        callback(ok);
    });
}

struct CheckoutResult {
    bool ok = false;
    QString token;
};

// Реальный POST /checkout к license-server-stub — раньше воркер только
// пинговал её /health, ни разу не занимая и не освобождая место в пуле, то
// есть ограничение на число лицензий (LICENSE_SEATS) фактически ни на что
// не влияло. 503 от checkout — легитимный business-ответ "мест нет сейчас",
// а не ошибка сервиса, поэтому здесь не reject/DLQ с ходу, а retry с
// бэкоффом (см. вызывающий код): место должно освободиться по TTL.
void checkoutLicense(QNetworkAccessManager &nam, const QUrl &licenseServerUrl, const QString &clientId,
                      const QString &jobId, std::function<void(CheckoutResult)> callback) {
    QUrl url = licenseServerUrl;
    url.setPath(url.path() + "/checkout");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    const QByteArray body = QJsonDocument(QJsonObject{{"client_id", clientId}, {"job_id", jobId}}).toJson(QJsonDocument::Compact);
    auto *reply = nam.post(request, body);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(3000);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
        CheckoutResult result;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() == QNetworkReply::NoError && doc.isObject()) {
            result.ok = true;
            result.token = doc.object().value("token").toString();
        }
        reply->deleteLater();
        callback(result);
    });
}

// checkin отправляется "best effort": если он не пройдёт (сеть моргнула
// именно в этот момент), лицензия всё равно освободится сама по TTL на
// license-server-stub — не блокируем ack задания ради этого.
void checkinLicense(QNetworkAccessManager &nam, const QUrl &licenseServerUrl, const QString &token) {
    QUrl url = licenseServerUrl;
    url.setPath(url.path() + "/checkin");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const QByteArray body = QJsonDocument(QJsonObject{{"token", token}}).toJson(QJsonDocument::Compact);
    auto *reply = nam.post(request, body);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[nx-worker-stub] license checkin failed (will expire by TTL anyway):" << reply->errorString();
        }
        reply->deleteLater();
    });
}

// Публикует событие о смене статуса задачи (processing/done/failed) в
// fanout jobs.status, которое подхватывает job-orchestrator и переносит в
// PostgreSQL. Сам воркер таблицу jobs никогда не трогает — единственный
// владелец БД в системе остаётся job-orchestrator, см. его README.
//
// Обменник называется jobs.status, а не jobs.completed (как было раньше) —
// он теперь несёт не только финальные события (done/failed), но и
// промежуточное processing, так что старое имя стало вводить в заблуждение.
void publishStatus(QtAmqpConsumer &amqp, const QString &jobId, const QString &status,
                    const QString &resultFileRef = {}, const QString &errorMessage = {}) {
    QJsonObject payload{{"job_id", jobId}, {"status", status}};
    if (!resultFileRef.isEmpty()) payload["result_file_ref"] = resultFileRef;
    if (!errorMessage.isEmpty()) payload["error_message"] = errorMessage;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!amqp.publish("jobs.status", "", body)) {
        qWarning() << "[nx-worker-stub] failed to publish" << status << "event for job" << jobId;
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    const QString serviceName = "nx-worker-stub";

    const QUrl rabbitmqUrl(qEnvironmentVariable("RABBITMQ_URL", "amqp://simensnx:simensnx@rabbitmq:5672/"));
    if (!rabbitmqUrl.isValid() || rabbitmqUrl.host().isEmpty()) {
        qCritical() << "[" << serviceName << "] invalid RABBITMQ_URL:" << rabbitmqUrl;
        return 1;
    }

    const QUrl fileStorageUrl(qEnvironmentVariable("FILE_STORAGE_URL", "http://file-storage-service:8084"));
    const QUrl licenseServerUrl(qEnvironmentVariable("LICENSE_SERVER_URL", "http://license-server-stub:8083"));

    bool prefetchOk = false;
    const int prefetchCount = qEnvironmentVariable("WORKER_PREFETCH", "1").toInt(&prefetchOk);

    QNetworkAccessManager nam;
    QtAmqpConsumer amqp;
    Metrics metrics;

    // job_id -> (queue, deliveryTag) для лога при завершении. deliveryTag
    // сам по себе уникален только в рамках канала, поэтому job_id как ключ
    // здесь чисто для читаемости логов, а не для корректности ack.
    QObject::connect(&amqp, &QtAmqpConsumer::connectionError, [&](const QString &msg) {
        qWarning() << "[" << serviceName << "] RabbitMQ error:" << msg;
    });

    auto handleMessage = [&](const QString &queueName) {
        return [&amqp, &nam, &metrics, fileStorageUrl, licenseServerUrl, serviceName, queueName](
                   const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
            const QByteArray body(message.body(), static_cast<int>(message.bodySize()));

            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                qWarning() << "[" << serviceName << "] malformed message on" << queueName
                           << "- rejecting without requeue:" << parseError.errorString();
                amqp.reject(deliveryTag, false);
                return;
            }

            const QJsonObject obj = doc.object();
            const QString jobId = obj.value("job_id").toString();
            const QString clientId = obj.value("client_id").toString();
            const QString inputFileRef = obj.value("input_file_ref").toString();

            if (jobId.isEmpty() || clientId.isEmpty() || inputFileRef.isEmpty()) {
                qWarning() << "[" << serviceName << "] message missing required fields on" << queueName
                           << "- rejecting without requeue, payload:" << QString::fromUtf8(body);
                amqp.reject(deliveryTag, false);
                return;
            }

            qInfo() << "[" << serviceName << "] picked up job" << jobId << "from" << queueName
                    << "(redelivered=" << redelivered << ")";
            metrics.inc("worker_jobs_picked_up_total", "Total messages picked up from work queues", {{"queue", queueName}});

            // processing публикуется ДО начала любой работы — это то самое
            // недостающее звено между 'queued' и 'done'/'failed', которое
            // раньше нигде не проставлялось, хотя колонка под него была в
            // схеме с самого начала (db/migrations/0001_init.sql). Заодно
            // это событие, по которому job-orchestrator считает попытки
            // (attempt_count) — см. его main.cpp.
            publishStatus(amqp, jobId, "processing");

            if (queueName == "jobs.validate") {
                // По требованию: реальной валидации на этом этапе нет,
                // задача просто одобряется. Никаких вызовов license-server/
                // file-storage — валидации нечего у них спрашивать.
                // Настоящая проверка геометрии/формата появится вместе с
                // реальным NX-воркером (Phase 2 ROADMAP.md); пока это
                // сознательная заглушка, а не забытый TODO.
                qInfo() << "[" << serviceName << "] job" << jobId << "auto-approved (validate is a stub), acking";
                amqp.ack(deliveryTag);
                metrics.inc("worker_jobs_completed_total", "Total jobs finished by the worker", {{"job_type", "validate"}, {"result", "done"}});
                publishStatus(amqp, jobId, "done");
                return;
            }

            // jobs.process — теперь реально занимает место в пуле лицензий
            // перед "обработкой" и освобождает его после, а не просто
            // пингует license-server-stub. checkoutAttempt рекурсивно
            // ретраит с бэкоффом при "мест нет" (503) — это легитимное
            // временное состояние (см. комментарий в checkoutLicense), а не
            // повод сразу проваливать задание.
            auto checkoutAttempt = std::make_shared<std::function<void(int)>>();
            *checkoutAttempt = [&amqp, &nam, &metrics, licenseServerUrl, fileStorageUrl, serviceName, jobId, clientId,
                                 deliveryTag, checkoutAttempt](int attempt) {
                checkoutLicense(nam, licenseServerUrl, clientId, jobId, [&amqp, &nam, &metrics, licenseServerUrl, fileStorageUrl,
                                                                          serviceName, jobId, deliveryTag,
                                                                          checkoutAttempt, attempt](CheckoutResult license) {
                    if (!license.ok) {
                        constexpr int kMaxCheckoutAttempts = 10;
                        if (attempt >= kMaxCheckoutAttempts) {
                            qWarning() << "[" << serviceName << "] job" << jobId
                                       << "could not obtain a license seat after" << attempt
                                       << "attempts - giving up, routing to DLQ";
                            metrics.inc("worker_jobs_completed_total", "Total jobs finished by the worker", {{"job_type", "process"}, {"result", "failed"}});
                            amqp.reject(deliveryTag, false); // -> jobs.dlq через RabbitMQ policy
                            publishStatus(amqp, jobId, "failed", {}, "no license seat available after retries");
                            // *checkoutAttempt хранит лямбду, которая сама
                            // держит копию checkoutAttempt — самоссылающийся
                            // shared_ptr<function> цикл. Одного reset()
                            // локальной копии недостаточно, чтобы его
                            // разорвать; нужно обнулить сам std::function.
                            *checkoutAttempt = nullptr;
                            return;
                        }
                        const int delayMs = std::min(1000 * (1 << attempt), 15000);
                        qInfo() << "[" << serviceName << "] job" << jobId << "no license seat yet, retry"
                                << (attempt + 1) << "in" << delayMs << "ms";
                        metrics.inc("worker_license_checkout_retries_total", "Total times a license checkout had to be retried because no seat was free");
                        QTimer::singleShot(delayMs, [checkoutAttempt, attempt]() { (*checkoutAttempt)(attempt + 1); });
                        return;
                    }

                    // Лицензия получена — дальше цикл ретраев больше не
                    // понадобится, рвём тот же self-reference cycle.
                    *checkoutAttempt = nullptr;

                    checkDependency(nam, fileStorageUrl, "file-storage-service",
                                     [&amqp, &nam, &metrics, licenseServerUrl, serviceName, jobId, deliveryTag, token = license.token](bool) {
                        // Имитация обработки задания. Намеренно не блокирует
                        // event loop (в отличие от std::this_thread::sleep_for
                        // в исходной заглушке) — на реальном Windows-воркере
                        // здесь будет вызов NX API.
                        QTimer::singleShot(1000, [&amqp, &nam, &metrics, licenseServerUrl, serviceName, jobId, deliveryTag, token]() {
                            qInfo() << "[" << serviceName << "] job" << jobId << "processed, releasing license and acking";
                            checkinLicense(nam, licenseServerUrl, token);
                            amqp.ack(deliveryTag);
                            metrics.inc("worker_jobs_completed_total", "Total jobs finished by the worker", {{"job_type", "process"}, {"result", "done"}});
                            // result_file_ref — плейсхолдер-ключ в
                            // file-storage-service; реальная запись файла
                            // результата появится вместе с настоящей NX-логикой.
                            publishStatus(amqp, jobId, "done", "results/" + jobId + ".step");
                        });
                    });
                });
            };
            (*checkoutAttempt)(0);
        };
    };

    QObject::connect(&amqp, &QtAmqpConsumer::ready, [&]() {
        qInfo() << "[" << serviceName << "] RabbitMQ channel ready, starting consumers";
        amqp.declareCompletionExchange("jobs.status");
        const int prefetch = prefetchOk && prefetchCount > 0 ? prefetchCount : 1;
        amqp.consume("jobs.process", prefetch, handleMessage("jobs.process"));
        amqp.consume("jobs.validate", prefetch, handleMessage("jobs.validate"));
    });

    amqp.connectToServer(rabbitmqUrl);

    // У воркера раньше вообще не было HTTP — чистый consumer. Добавлен
    // минимальный сервер только ради /health (liveness-проба) и /metrics
    // (Prometheus scrape); никакой бизнес-логики через HTTP сюда не
    // приходит и не должно.
    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8085").toUShort(&portOk);
    HttpServer httpServer;
    httpServer.addRoute("GET", "/health", [&amqp](const HttpRequest &) {
        if (!amqp.isReady()) {
            return HttpResponse::json(503, "Service Unavailable", R"({"status":"degraded","rabbitmq":"not_ready"})");
        }
        return HttpResponse::json(200, "OK", R"({"status":"healthy","rabbitmq":"ready"})");
    });
    httpServer.addRoute("GET", "/metrics", [&metrics](const HttpRequest &) {
        HttpResponse resp;
        resp.statusCode = 200;
        resp.statusText = "OK";
        resp.contentType = "text/plain; version=0.0.4";
        resp.body = metrics.render();
        return resp;
    });
    if (portOk && !httpServer.listen(QHostAddress::AnyIPv4, port)) {
        qWarning() << "[" << serviceName << "] failed to listen on port" << port << "- /health and /metrics unavailable";
    }

    qInfo() << "[" << serviceName << "] started, connecting to RabbitMQ at" << rabbitmqUrl.host();
    return app.exec();
}

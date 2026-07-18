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

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

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
// file-storage-service и license-server-stub на момент написания этого
// сервиса ЕЩЁ САМИ являются пустыми заглушками (см. отдельные тикеты на их
// доработку) — поэтому здесь ожидаемо будет connection refused, и это не
// баг воркера. Как только эти два сервиса научатся слушать порт и отвечать
// на /health, эти проверки начнут проходить без единой правки этого файла.
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

// Публикует событие о завершении задачи в fanout jobs.completed, которое
// подхватывает job-orchestrator и переносит в PostgreSQL (UPDATE jobs SET
// status=...). Сам воркер таблицу jobs никогда не трогает — единственный
// владелец БД в системе остаётся job-orchestrator, см. его README.
void publishCompletion(QtAmqpConsumer &amqp, const QString &jobId, const QString &status,
                        const QString &resultFileRef = {}) {
    QJsonObject payload{{"job_id", jobId}, {"status", status}};
    if (!resultFileRef.isEmpty()) payload["result_file_ref"] = resultFileRef;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!amqp.publish("jobs.completed", "", body)) {
        qWarning() << "[nx-worker-stub] failed to publish completion event for job" << jobId;
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

    // job_id -> (queue, deliveryTag) для лога при завершении. deliveryTag
    // сам по себе уникален только в рамках канала, поэтому job_id как ключ
    // здесь чисто для читаемости логов, а не для корректности ack.
    QObject::connect(&amqp, &QtAmqpConsumer::connectionError, [&](const QString &msg) {
        qWarning() << "[" << serviceName << "] RabbitMQ error:" << msg;
    });

    auto handleMessage = [&](const QString &queueName) {
        return [&amqp, &nam, fileStorageUrl, licenseServerUrl, serviceName, queueName](
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

            if (queueName == "jobs.validate") {
                // По требованию: реальной валидации на этом этапе нет,
                // задача просто одобряется. Никаких вызовов license-server/
                // file-storage — валидации нечего у них спрашивать.
                // Настоящая проверка геометрии/формата появится вместе с
                // реальным NX-воркером (Phase 2 ROADMAP.md); пока это
                // сознательная заглушка, а не забытый TODO.
                qInfo() << "[" << serviceName << "] job" << jobId << "auto-approved (validate is a stub), acking";
                amqp.ack(deliveryTag);
                publishCompletion(amqp, jobId, "done");
                return;
            }

            // jobs.process — полная цепочка проверки связности перед
            // "обработкой" (см. комментарий в checkDependency).
            checkDependency(nam, licenseServerUrl, "license-server-stub", [&amqp, &nam, fileStorageUrl, serviceName, jobId, deliveryTag](bool) {
                checkDependency(nam, fileStorageUrl, "file-storage-service", [&amqp, serviceName, jobId, deliveryTag](bool) {
                    // Имитация обработки задания. Намеренно не блокирует
                    // event loop (в отличие от std::this_thread::sleep_for
                    // в исходной заглушке) — на реальном Windows-воркере
                    // здесь будет вызов NX API.
                    QTimer::singleShot(1000, [&amqp, serviceName, jobId, deliveryTag]() {
                        qInfo() << "[" << serviceName << "] job" << jobId << "processed, acking";
                        amqp.ack(deliveryTag);
                        // result_file_ref — плейсхолдер-ключ в
                        // file-storage-service; реальная запись файла
                        // результата появится вместе с настоящей NX-логикой.
                        publishCompletion(amqp, jobId, "done", "results/" + jobId + ".step");
                    });
                });
            });
        };
    };

    QObject::connect(&amqp, &QtAmqpConsumer::ready, [&]() {
        qInfo() << "[" << serviceName << "] RabbitMQ channel ready, starting consumers";
        amqp.declareCompletionExchange("jobs.completed");
        const int prefetch = prefetchOk && prefetchCount > 0 ? prefetchCount : 1;
        amqp.consume("jobs.process", prefetch, handleMessage("jobs.process"));
        amqp.consume("jobs.validate", prefetch, handleMessage("jobs.validate"));
    });

    amqp.connectToServer(rabbitmqUrl);

    qInfo() << "[" << serviceName << "] started, connecting to RabbitMQ at" << rabbitmqUrl.host();
    return app.exec();
}

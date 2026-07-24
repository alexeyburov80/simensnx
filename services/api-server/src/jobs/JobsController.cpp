#include "JobsController.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUuid>
#include <QDebug>

#include "JobPublisher.h"
#include "auth/AuthStubClient.h"
#include "storage/jobrepository.h"
#include "Metrics.h"

namespace {

QByteArray errorJson(const QString &message) {
    QJsonObject obj{{"error", message}};
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace

JobsController::JobsController(IJobPublisher &publisher, JobRepository &jobRepository,
                                AuthStubClient &authClient, Metrics &metrics, RabbitConfig rabbitConfig)
    : m_publisher(publisher), m_jobRepository(jobRepository), m_authClient(authClient),
      m_metrics(metrics), m_rabbitConfig(std::move(rabbitConfig))
{
}

void JobsController::handleCreate(const HttpRequest &req, HttpServer::RespondFn respond) const {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(req.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        respond(HttpResponse::json(400, "Bad Request", errorJson("invalid JSON body: " + parseError.errorString())));
        return;
    }

    const QJsonObject obj = doc.object();

    // Контракт приведён в соответствие со схемой jobs (db/migrations/0001_init.sql):
    // client_id и input_file_ref там NOT NULL, а поля model_id в схеме
    // вообще нет — использовать его для job-state-service было бы нечем.
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

    // &m_publisher/&m_jobRepository/&m_metrics/m_rabbitConfig — this (JobsController)
    // переживает вызов respond() в производственном коде (владеет им HttpServer
    // маршрутизация, время жизни которой больше запроса), так что захват this
    // безопасен так же, как раньше был безопасен захват &amqp/&metrics по ссылке
    // из main() — тот же самый жизненный цикл, просто теперь через объект.
    m_authClient.verify(authHeader, [this, respond, clientId, jobType, inputFileRef, idempotencyKey]
                         (bool allowed, QString authError) {
        if (!allowed) {
            // auth-stub недоступен или отказал — fail closed, а не
            // тихо публикуем без проверки (это свело бы на нет весь
            // смысл её вызова).
            qWarning() << "[api-server] auth check failed:" << authError;
            m_metrics.inc("api_auth_check_failures_total", "Total requests rejected because auth-stub was unavailable or denied");
            respond(HttpResponse::json(503, "Service Unavailable",
                                        errorJson("auth service unavailable: " + authError)));
            return;
        }

        // Идемпотентность ДО публикации, а не после (как было раньше,
        // когда job-state-service ловил дубль постфактум в БД, а в
        // очередь уже улетали два разных job_id на одну и ту же
        // клиентскую отправку). Теперь при повторе с тем же
        // idempotency_key ничего заново не публикуется — клиенту
        // возвращается тот же job_id, что и в первый раз.
        if (!idempotencyKey.isEmpty()) {
            const ExistingJob existing = m_jobRepository.findByIdempotencyKey(idempotencyKey);
            if (existing.found) {
                m_metrics.inc("api_idempotent_replays_total", "Total POST /jobs requests that matched an existing idempotency_key");
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
        const QString queue = (jobType == "validate") ? m_rabbitConfig.validateQueue : m_rabbitConfig.processQueue;

        if (!m_publisher.publish("", queue, message)) {
            // Канал не готов — сообщение НЕ ушло. Раньше publish_job()
            // в этом случае молча ничего не делал, а клиенту всё равно
            // возвращался "status": "pending", как будто всё в порядке.
            m_metrics.inc("api_jobs_publish_failed_total", "Total POST /jobs requests that failed because the RabbitMQ channel was not ready",
                        {{"job_type", jobType}});
            respond(HttpResponse::json(503, "Service Unavailable",
                                        errorJson("RabbitMQ channel is not ready, job was not queued")));
            return;
        }
        m_metrics.inc("api_jobs_published_total", "Total jobs successfully published to a work queue", {{"job_type", jobType}});

        // Копия того же сообщения — в fanout jobs.events, чтобы
        // job-state-service узнал о задаче и сохранил её в PostgreSQL.
        // Не влияет на успешность основного publish выше: если эта
        // публикация не удастся, job всё равно уйдёт в обработку,
        // просто запись в БД появится с опозданием (или после ручной
        // сверки) — это сознательный компромисс Phase 0, где
        // retry-логики ещё нет.
        if (!m_publisher.publish(m_rabbitConfig.eventsExchange, "", message)) {
            qWarning() << "[api-server] failed to publish jobs.events copy for job" << jobId;
        }

        QJsonObject respObj{{"job_id", jobId}, {"status", "queued"}, {"queue", queue}};
        respond(HttpResponse::json(202, "Accepted", QJsonDocument(respObj).toJson(QJsonDocument::Compact)));
    });
}

HttpResponse JobsController::handleGet(const HttpRequest &req) const {
    // Реальное чтение из Postgres — раньше здесь всегда возвращался
    // фиктивный "status": "unknown". job-state-service теперь пишет
    // состояние по-настоящему (см. его README), так что читать можно.
    const JobLookup lookup = m_jobRepository.findById(req.pathParams.value(0));

    if (lookup.queryFailed) {
        return HttpResponse::json(500, "Internal Server Error", errorJson("database query failed"));
    }
    if (!lookup.found) {
        return HttpResponse::json(404, "Not Found", errorJson("job not found"));
    }

    QJsonObject obj;
    obj["job_id"] = lookup.fields.value("id").toString();
    obj["client_id"] = lookup.fields.value("client_id").toString();
    obj["job_type"] = lookup.fields.value("job_type").toString();
    obj["status"] = lookup.fields.value("status").toString();
    obj["input_file_ref"] = lookup.fields.value("input_file_ref").toString();
    const QVariant resultRef = lookup.fields.value("result_file_ref");
    if (!resultRef.isNull()) obj["result_file_ref"] = resultRef.toString();
    const QVariant errMsg = lookup.fields.value("error_message");
    if (!errMsg.isNull()) obj["error_message"] = errMsg.toString();
    obj["attempt_count"] = lookup.fields.value("attempt_count").toInt();
    obj["max_attempts"] = lookup.fields.value("max_attempts").toInt();
    obj["created_at"] = lookup.fields.value("created_at").toDateTime().toUTC().toString(Qt::ISODate);
    obj["updated_at"] = lookup.fields.value("updated_at").toDateTime().toUTC().toString(Qt::ISODate);
    return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

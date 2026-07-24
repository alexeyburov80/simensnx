#include <QtTest>
#include <QEventLoop>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>

#include "jobs/JobsController.h"
#include "auth/AuthStubClient.h"
#include "storage/jobrepository.h"
#include "Metrics.h"
#include "JobPublisher.h"
#include "HttpServer.h"

namespace {

// До выноса в JobsController эта логика жила прямо в main() как
// 111-строчная лямбда обработчика POST /jobs — протестировать её было
// нельзя в принципе, не поднимая весь процесс целиком. Вот эти тесты и
// есть доказательство, что вынос того стоил: реальная бизнес-логика,
// реальный SQL (через SQLite), реальный HTTP-вызов auth-stub (через
// локальный HttpServer-двойник) — и всё это без единой реальной сети
// наружу и без amqpcpp.

// Фейковый publisher — записывает все вызовы publish() для проверки в
// тестах, вместо реального подключения к RabbitMQ (которое и так
// сознательно исключено из api-server-core, см. CMakeLists.txt).
class FakeJobPublisher : public IJobPublisher {
public:
    struct Call {
        QString exchange;
        QString routingKey;
        QByteArray body;
    };
    QVector<Call> calls;
    QVector<bool> results; // results[i] — что вернёт i-й по счёту вызов publish(); если индекс вышел за пределы — true
    bool readyFlag = true;

    bool publish(const QString &exchange, const QString &routingKey, const QByteArray &body) override {
        const int idx = calls.size();
        calls.push_back({exchange, routingKey, body});
        return idx < results.size() ? results[idx] : true;
    }
    bool isReady() const override { return readyFlag; }
};

// Локальный HTTP-двойник auth-stub — тот же HttpServer, что используется
// в проде (см. tst_httpserver.cpp), просто с одним маршрутом POST /verify,
// поведение которого настраивается на лету через captured bool.
std::unique_ptr<HttpServer> startFakeAuthServer(bool allowed, quint16 &outPort) {
    auto server = std::make_unique<HttpServer>();
    server->addRoute("POST", "/verify", [allowed](const HttpRequest &) -> HttpResponse {
        return HttpResponse::json(200, "OK", allowed ? R"({"allowed":true})" : R"({"allowed":false})");
    });
    server->listen(QHostAddress::LocalHost, 0);
    outPort = server->port();
    return server;
}

// handleCreate асинхронный (respond() может быть вызван позже, после
// ответа auth-stub) — но может быть вызван и СИНХРОННО (валидационные
// ошибки 400 отвечают немедленно). Поэтому: сначала проверяем, не
// ответили ли уже, и только если нет — крутим event loop с таймаутом.
HttpResponse runCreate(const JobsController &controller, const HttpRequest &req, int timeoutMs = 3000) {
    HttpResponse result;
    bool done = false;
    controller.handleCreate(req, [&](HttpResponse resp) {
        result = resp;
        done = true;
    });
    if (!done) {
        QEventLoop loop;
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        // handleCreate уже поставил колбэк в очередь событий (запрос к
        // auth-stub) — donёт true, когда он реально вызовется; оборачиваем
        // в ещё один QTimer::singleShot(0, ...), чтобы выйти из loop.exec()
        // сразу, как только done станет true, а не только по таймауту.
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (done) loop.quit(); });
        poll.start(10);
        loop.exec();
    }
    return result;
}

HttpRequest makeCreateRequest(const QByteArray &jsonBody) {
    HttpRequest req;
    req.method = "POST";
    req.path = "/jobs";
    req.body = jsonBody;
    return req;
}

} // namespace

class TstJobsController : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
        db.setDatabaseName(":memory:");
        QVERIFY2(db.open(), qPrintable(db.lastError().text()));

        QSqlQuery q;
        QVERIFY2(q.exec("CREATE TABLE jobs ("
                         "  id TEXT PRIMARY KEY,"
                         "  idempotency_key TEXT,"
                         "  client_id TEXT,"
                         "  job_type TEXT,"
                         "  status TEXT,"
                         "  input_file_ref TEXT,"
                         "  result_file_ref TEXT,"
                         "  error_message TEXT,"
                         "  attempt_count INTEGER,"
                         "  max_attempts INTEGER,"
                         "  created_at TEXT,"
                         "  updated_at TEXT"
                         ")"),
                 qPrintable(q.lastError().text()));
    }

    void cleanupTestCase() {
        QSqlDatabase::removeDatabase(QSqlDatabase::database().connectionName());
    }

    void init() {
        QSqlQuery q;
        QVERIFY(q.exec("DELETE FROM jobs"));
    }

    // --- Валидация тела запроса (все синхронные — до вызова auth-stub) ---

    void create_invalidJson_returns400WithoutCallingAuthOrPublisher() {
        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl("http://127.0.0.1:1")); // недостижимый порт — если дойдёт сюда, тест зависнет и упадёт по таймауту QTest
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const HttpResponse resp = runCreate(controller, makeCreateRequest("not json"));
        QCOMPARE(resp.statusCode, 400);
        QVERIFY(publisher.calls.isEmpty());
    }

    void create_missingClientId_returns400() {
        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl("http://127.0.0.1:1"));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"job_type":"process","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 400);
        QVERIFY(resp.body.contains("client_id"));
    }

    void create_invalidJobType_returns400() {
        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl("http://127.0.0.1:1"));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"nonsense","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 400);
    }

    // --- auth-stub ---

    void create_authDenies_returns503WithoutPublishing() {
        quint16 authPort = 0;
        auto authServer = startFakeAuthServer(/*allowed=*/false, authPort);

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl(QString("http://127.0.0.1:%1").arg(authPort)));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"process","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 503);
        QVERIFY(publisher.calls.isEmpty()); // fail closed — до публикации дело не дошло
    }

    void create_authUnreachable_returns503() {
        QNetworkAccessManager nam;
        // Порт 1 на loopback — ничего не слушает, connection refused
        // приходит быстро, не нужно ждать 3-секундный таймаут AuthStubClient.
        AuthStubClient auth(nam, QUrl("http://127.0.0.1:1"));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"process","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body), 5000);
        QCOMPARE(resp.statusCode, 503);
        QVERIFY(publisher.calls.isEmpty());
    }

    // --- идемпотентность ---

    void create_existingIdempotencyKey_returns200ReplayWithoutPublishing() {
        QSqlQuery insert;
        insert.prepare("INSERT INTO jobs (id, idempotency_key, status) VALUES (:id, :key, :status)");
        insert.bindValue(":id", "33333333-3333-3333-3333-333333333333");
        insert.bindValue(":key", "order-42");
        insert.bindValue(":status", "done");
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));

        quint16 authPort = 0;
        auto authServer = startFakeAuthServer(/*allowed=*/true, authPort);

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl(QString("http://127.0.0.1:%1").arg(authPort)));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"process","input_file_ref":"x.step","idempotency_key":"order-42"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 200);
        QVERIFY(publisher.calls.isEmpty()); // повтор — публикации в очередь быть не должно

        const QJsonObject respObj = QJsonDocument::fromJson(resp.body).object();
        QCOMPARE(respObj.value("job_id").toString(), QString("33333333-3333-3333-3333-333333333333"));
        QVERIFY(respObj.value("idempotent_replay").toBool());
    }

    // --- happy path и маршрутизация по job_type ---

    void create_validateJobType_publishesToValidateQueueAndEventsExchange() {
        quint16 authPort = 0;
        auto authServer = startFakeAuthServer(/*allowed=*/true, authPort);

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl(QString("http://127.0.0.1:%1").arg(authPort)));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        RabbitConfig rabbitConfig; // дефолты: processQueue="jobs.process", validateQueue="jobs.validate", eventsExchange="jobs.events"
        JobsController controller(publisher, repo, auth, metrics, rabbitConfig);

        const QByteArray body = R"({"client_id":"acme","job_type":"validate","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 202);

        QCOMPARE(publisher.calls.size(), 2);
        QCOMPARE(publisher.calls[0].routingKey, QString("jobs.validate")); // основная работа — в work-очередь
        QCOMPARE(publisher.calls[1].exchange, QString("jobs.events"));      // копия — в fanout для job-state-service

        const QJsonObject respObj = QJsonDocument::fromJson(resp.body).object();
        QCOMPARE(respObj.value("status").toString(), QString("queued"));
        QCOMPARE(respObj.value("queue").toString(), QString("jobs.validate"));
        QVERIFY(!respObj.value("job_id").toString().isEmpty());
    }

    void create_processJobType_publishesToProcessQueue() {
        quint16 authPort = 0;
        auto authServer = startFakeAuthServer(/*allowed=*/true, authPort);

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl(QString("http://127.0.0.1:%1").arg(authPort)));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"process","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 202);
        QCOMPARE(publisher.calls[0].routingKey, QString("jobs.process"));
    }

    // --- канал не готов ---

    void create_publisherNotReady_returns503() {
        quint16 authPort = 0;
        auto authServer = startFakeAuthServer(/*allowed=*/true, authPort);

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl(QString("http://127.0.0.1:%1").arg(authPort)));
        JobRepository repo;
        FakeJobPublisher publisher;
        publisher.results = {false}; // первый publish (в work-очередь) проваливается
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"process","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 503);
        QCOMPARE(publisher.calls.size(), 1); // до второй публикации (jobs.events) дело не дошло
    }

    // Публикация копии в jobs.events — best-effort: её неудача НЕ должна
    // превращать успешный ответ в ошибку (см. комментарий в
    // JobsController.cpp про "сознательный компромисс Phase 0").
    void create_eventsExchangePublishFails_stillReturns202() {
        quint16 authPort = 0;
        auto authServer = startFakeAuthServer(/*allowed=*/true, authPort);

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl(QString("http://127.0.0.1:%1").arg(authPort)));
        JobRepository repo;
        FakeJobPublisher publisher;
        publisher.results = {true, false}; // основная публикация ок, копия в jobs.events — нет
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        const QByteArray body = R"({"client_id":"acme","job_type":"process","input_file_ref":"x.step"})";
        const HttpResponse resp = runCreate(controller, makeCreateRequest(body));
        QCOMPARE(resp.statusCode, 202);
        QCOMPARE(publisher.calls.size(), 2);
    }

    // --- GET /jobs/{id} ---

    void get_unknownId_returns404() {
        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl("http://127.0.0.1:1"));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        HttpRequest req;
        req.pathParams = QStringList{"does-not-exist"};
        const HttpResponse resp = controller.handleGet(req);
        QCOMPARE(resp.statusCode, 404);
    }

    void get_knownId_returns200WithFields() {
        QSqlQuery insert;
        insert.prepare(
            "INSERT INTO jobs (id, client_id, job_type, status, input_file_ref, attempt_count, max_attempts, created_at, updated_at) "
            "VALUES (:id, :client_id, :job_type, :status, :input_file_ref, :attempt_count, :max_attempts, :created_at, :updated_at)");
        insert.bindValue(":id", "44444444-4444-4444-4444-444444444444");
        insert.bindValue(":client_id", "acme");
        insert.bindValue(":job_type", "process");
        insert.bindValue(":status", "queued");
        insert.bindValue(":input_file_ref", "x.step");
        insert.bindValue(":attempt_count", 0);
        insert.bindValue(":max_attempts", 3);
        insert.bindValue(":created_at", "2026-07-22T10:00:00");
        insert.bindValue(":updated_at", "2026-07-22T10:00:00");
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));

        QNetworkAccessManager nam;
        AuthStubClient auth(nam, QUrl("http://127.0.0.1:1"));
        JobRepository repo;
        FakeJobPublisher publisher;
        Metrics metrics;
        JobsController controller(publisher, repo, auth, metrics, RabbitConfig{});

        HttpRequest req;
        req.pathParams = QStringList{"44444444-4444-4444-4444-444444444444"};
        const HttpResponse resp = controller.handleGet(req);
        QCOMPARE(resp.statusCode, 200);

        const QJsonObject obj = QJsonDocument::fromJson(resp.body).object();
        QCOMPARE(obj.value("client_id").toString(), QString("acme"));
        QCOMPARE(obj.value("status").toString(), QString("queued"));
        QVERIFY(!obj.contains("result_file_ref")); // NULL в БД -> поле опущено целиком, не null
    }
};

QTEST_GUILESS_MAIN(TstJobsController)
#include "tst_jobscontroller.moc"

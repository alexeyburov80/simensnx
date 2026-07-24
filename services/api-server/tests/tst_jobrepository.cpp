#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

#include "storage/jobrepository.h"

// JobRepository работает через QSqlQuery() без явного имени соединения —
// то есть всегда через дефолтное соединение QSqlDatabase. В проде это
// QPSQL, поднятый в main.cpp; здесь — QSQLITE in-memory с такой же формой
// таблицы jobs (только нужные для этого класса колонки), чтобы не тащить
// в юнит-тесты реальный PostgreSQL. Реальный SQL (плейсхолдеры, имя
// колонки) при этом проверяется по-настоящему, а не мокается — если кто-то
// опечатается в имени колонки в query.prepare(), тест упадёт.
class TstJobRepository : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE"); // без имени -> дефолтное соединение
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

    void findByIdempotencyKey_noMatch_returnsNotFound() {
        JobRepository repo;
        const ExistingJob result = repo.findByIdempotencyKey("does-not-exist");
        QVERIFY(!result.found);
    }

    void findByIdempotencyKey_match_returnsIdAndStatus() {
        QSqlQuery insert;
        insert.prepare("INSERT INTO jobs (id, idempotency_key, status) VALUES (:id, :key, :status)");
        insert.bindValue(":id", "11111111-1111-1111-1111-111111111111");
        insert.bindValue(":key", "client-key-42");
        insert.bindValue(":status", "queued");
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));

        JobRepository repo;
        const ExistingJob result = repo.findByIdempotencyKey("client-key-42");
        QVERIFY(result.found);
        QCOMPARE(result.jobId, QString("11111111-1111-1111-1111-111111111111"));
        QCOMPARE(result.status, QString("queued"));
    }

    // Ключ идемпотентности не уникален на уровне SQL-запроса в этом
    // классе (UNIQUE-ограничение — забота схемы БД, не этого кода) —
    // findByIdempotencyKey() должен вернуть именно первую найденную
    // строку, а не упасть и не вернуть последнюю.
    void findByIdempotencyKey_multipleMatches_returnsFirstRow() {
        QSqlQuery insert;
        insert.prepare("INSERT INTO jobs (id, idempotency_key, status) VALUES (:id, :key, :status)");
        for (const auto &pair : {std::pair{"aaaa", "queued"}, std::pair{"bbbb", "done"}}) {
            insert.bindValue(":id", pair.first);
            insert.bindValue(":key", "dup-key");
            insert.bindValue(":status", pair.second);
            QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
        }

        JobRepository repo;
        const ExistingJob result = repo.findByIdempotencyKey("dup-key");
        QVERIFY(result.found);
        QCOMPARE(result.jobId, QString("aaaa"));
    }

    // Пустой ключ — не особый случай на уровне SQL (это просто ':key' = '')
    // — но именно на этой границе main.cpp принимает решение, вызывать ли
    // findByIdempotencyKey() вообще (см. "if (!idempotencyKey.isEmpty())"
    // перед вызовом в POST /jobs). Здесь фиксируем поведение самого
    // репозитория, если его всё-таки вызовут с пустой строкой: он не
      // не должен по ошибке смэтчить строку с NULL idempotency_key.
    void findByIdempotencyKey_emptyKey_doesNotMatchNullColumn() {
        QSqlQuery insert;
        insert.prepare("INSERT INTO jobs (id, idempotency_key, status) VALUES (:id, NULL, :status)");
        insert.bindValue(":id", "cccc");
        insert.bindValue(":status", "queued");
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));

        JobRepository repo;
        const ExistingJob result = repo.findByIdempotencyKey("");
        QVERIFY(!result.found);
    }

    // findById() был объявлен в заголовке, но не реализован (ноль
    // вызывающих) до вынесения GET /jobs/{id} в JobsController — вот эти
    // тесты и есть первая проверка того, что реализация вообще делает то,
    // что нужно.
    void findById_noMatch_returnsNotFoundWithoutQueryFailure() {
        JobRepository repo;
        const JobLookup result = repo.findById("does-not-exist");
        QVERIFY(!result.found);
        QVERIFY(!result.queryFailed); // "не найдено" — не то же самое, что "запрос упал"
    }

    void findById_match_returnsAllFields() {
        QSqlQuery insert;
        insert.prepare(
            "INSERT INTO jobs (id, client_id, job_type, status, input_file_ref, result_file_ref, "
            "error_message, attempt_count, max_attempts, created_at, updated_at) "
            "VALUES (:id, :client_id, :job_type, :status, :input_file_ref, :result_file_ref, "
            ":error_message, :attempt_count, :max_attempts, :created_at, :updated_at)");
        insert.bindValue(":id", "22222222-2222-2222-2222-222222222222");
        insert.bindValue(":client_id", "acme-corp");
        insert.bindValue(":job_type", "process");
        insert.bindValue(":status", "done");
        insert.bindValue(":input_file_ref", "models/part1.step");
        insert.bindValue(":result_file_ref", "results/22222222.step");
        insert.bindValue(":error_message", QVariant(QVariant::String)); // NULL — задача не падала
        insert.bindValue(":attempt_count", 1);
        insert.bindValue(":max_attempts", 3);
        insert.bindValue(":created_at", "2026-07-22T10:00:00");
        insert.bindValue(":updated_at", "2026-07-22T10:00:05");
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));

        JobRepository repo;
        const JobLookup result = repo.findById("22222222-2222-2222-2222-222222222222");
        QVERIFY(result.found);
        QVERIFY(!result.queryFailed);
        QCOMPARE(result.fields.value("client_id").toString(), QString("acme-corp"));
        QCOMPARE(result.fields.value("status").toString(), QString("done"));
        QCOMPARE(result.fields.value("result_file_ref").toString(), QString("results/22222222.step"));
        QVERIFY(result.fields.value("error_message").isNull()); // JobsController::handleGet опускает это поле целиком, если NULL
        QCOMPARE(result.fields.value("attempt_count").toInt(), 1);
    }
};

QTEST_GUILESS_MAIN(TstJobRepository)
#include "tst_jobrepository.moc"

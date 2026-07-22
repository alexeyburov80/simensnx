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
                         "  status TEXT"
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
};

QTEST_GUILESS_MAIN(TstJobRepository)
#include "tst_jobrepository.moc"

#include <QtTest>

#include "HttpTypes.h"

class TstHttpTypes : public QObject {
    Q_OBJECT
private slots:
    // Значения по умолчанию — то, на что молча полагается весь остальной
    // код (например, SyncHandler-обёртка в HttpServer возвращает
    // HttpResponse{} для "ничего не делать" в паре мест по коду сервисов).
    // Если дефолты когда-нибудь поменяют не глядя, эти проверки должны
    // упасть первыми, а не только в проде на 500-й вместо 200-й ошибке.
    void defaultResponse_isOkJson() {
        HttpResponse resp;
        QCOMPARE(resp.statusCode, 200);
        QCOMPARE(resp.statusText, QByteArray("OK"));
        QCOMPARE(resp.contentType, QByteArray("application/json"));
        QVERIFY(resp.body.isEmpty());
    }

    void jsonFactory_setsAllFields() {
        const HttpResponse resp = HttpResponse::json(404, "Not Found", R"({"error":"not found"})");
        QCOMPARE(resp.statusCode, 404);
        QCOMPARE(resp.statusText, QByteArray("Not Found"));
        QCOMPARE(resp.contentType, QByteArray("application/json")); // json() не трогает contentType — он и так json
        QCOMPARE(resp.body, QByteArray(R"({"error":"not found"})"));
    }

    // HttpServer::tryProcess() кладёт заголовки в нижнем регистре
    // (req.headers.insert(key.toLower(), ...)) и потом читает их так же —
    // сам HttpRequest ничего не нормализует, это его контракт с вызывающим
    // кодом. Проверяем именно контракт: без принудительного lower() ключи
    // не совпадут, и это не баг структуры, а ответственность парсера.
    void headers_areCaseSensitiveContainer() {
        HttpRequest req;
        req.headers.insert("content-type", "application/json");
        QVERIFY(req.headers.contains("content-type"));
        QVERIFY(!req.headers.contains("Content-Type"));
    }

    void pathParams_defaultEmpty() {
        HttpRequest req;
        QVERIFY(req.pathParams.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstHttpTypes)
#include "tst_httptypes.moc"

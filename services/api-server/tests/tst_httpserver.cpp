#include <QtTest>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QEventLoop>
#include <QTimer>

#include "HttpServer.h"

namespace {

struct RawResponse {
    bool ok = false;
    int statusCode = -1;
    QByteArray statusText;
    QByteArray body;
    QByteArray full;
};

// HttpServer сознательно закрывает соединение после каждого ответа
// (Connection: close, см. комментарий в HttpServer.h) — это и есть сигнал
// "ответ пришёл целиком", которым мы здесь пользуемся вместо ручного
// парсинга Content-Length на стороне теста.
//
// Реализовано через QEventLoop + сигналы, а не через QTcpSocket::waitFor*():
// сервер (HttpServer) и клиентский сокет живут в одном потоке/приложении,
// а вложенные блокирующие waitFor*() не всегда успевают продвинуть accept
// нового соединения на стороне QTcpServer прежде, чем истечёт их локальный
// таймаут — событийный QEventLoop, реагирующий на сигналы обеих сторон,
// устойчивее.
RawResponse sendRawRequest(quint16 port, const QByteArray &rawRequest, int timeoutMs = 5000) {
    RawResponse result;
    QTcpSocket socket;

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QObject::connect(&socket, &QTcpSocket::connected, [&]() {
        socket.write(rawRequest);
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::errorOccurred, [&](QAbstractSocket::SocketError) {
        loop.quit();
    });

    socket.connectToHost(QHostAddress::LocalHost, port);
    timeoutTimer.start(timeoutMs);
    loop.exec();

    result.full = socket.readAll();
    if (result.full.isEmpty()) return result;

    static const QRegularExpression statusLineRe(R"(^HTTP/1\.1 (\d{3}) ([^\r\n]*)\r\n)");
    const auto m = statusLineRe.match(QString::fromLatin1(result.full));
    if (!m.hasMatch()) return result;

    result.statusCode = m.captured(1).toInt();
    result.statusText = m.captured(2).toLatin1();
    const int headerEnd = result.full.indexOf("\r\n\r\n");
    result.body = headerEnd >= 0 ? result.full.mid(headerEnd + 4) : QByteArray();
    result.ok = true;
    return result;
}

} // namespace

class TstHttpServer : public QObject {
    Q_OBJECT
private slots:
    void init() {
        m_server = std::make_unique<HttpServer>();
        // port=0 -> ОС выдаёт свободный ephemeral-порт, тесты не зависят
        // от того, что где-то ещё занят фиксированный порт.
        QVERIFY(m_server->listen(QHostAddress::LocalHost, 0));
        m_port = m_server->port();
        QVERIFY(m_port != 0);
    }

    void cleanup() {
        m_server.reset();
    }

    void exactRoute_get_returnsRegisteredResponse() {
        m_server->addRoute("GET", "/health", [](const HttpRequest &) -> HttpResponse {
            return HttpResponse::json(200, "OK", R"({"status":"healthy"})");
        });

        const RawResponse resp = sendRawRequest(m_port, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 200);
        QCOMPARE(resp.body, QByteArray(R"({"status":"healthy"})"));
    }

    void unregisteredPath_returns404() {
        const RawResponse resp = sendRawRequest(m_port, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 404);
    }

    void registeredPathWrongMethod_returns405() {
        m_server->addRoute("GET", "/jobs", [](const HttpRequest &) -> HttpResponse {
            return HttpResponse::json(200, "OK", "{}");
        });

        const RawResponse resp = sendRawRequest(m_port, "DELETE /jobs HTTP/1.1\r\nHost: x\r\n\r\n");
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 405);
    }

    void patternRoute_capturesPathParamAndIsUsableByHandler() {
        static const QRegularExpression jobIdPattern(R"(^/jobs/([0-9a-f-]{36})$)");
        m_server->addRoutePattern("GET", jobIdPattern, [](const HttpRequest &req) -> HttpResponse {
            const QString capturedId = req.pathParams.value(0);
            return HttpResponse::json(200, "OK", ("{\"job_id\":\"" + capturedId + "\"}").toUtf8());
        });

        const QString jobId = "11111111-1111-1111-1111-111111111111";
        const RawResponse resp = sendRawRequest(m_port, ("GET /jobs/" + jobId + " HTTP/1.1\r\nHost: x\r\n\r\n").toUtf8());
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 200);
        QVERIFY(resp.body.contains(jobId.toUtf8()));
    }

    void postWithBody_bodyReachesHandlerIntact() {
        m_server->addRoute("POST", "/echo", [](const HttpRequest &req) -> HttpResponse {
            return HttpResponse::json(200, "OK", req.body);
        });

        const QByteArray body = R"({"client_id":"abc","job_type":"process"})";
        QByteArray raw = "POST /echo HTTP/1.1\r\nHost: x\r\n";
        raw += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        raw += body;

        const RawResponse resp = sendRawRequest(m_port, raw);
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 200);
        QCOMPARE(resp.body, body);
    }

    void malformedRequestLine_returns400() {
        // Ни метода, ни пути — только мусор до \r\n\r\n.
        const RawResponse resp = sendRawRequest(m_port, "GARBAGE\r\n\r\n");
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 400);
    }

    void invalidContentLength_returns400() {
        const RawResponse resp = sendRawRequest(m_port, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: not-a-number\r\n\r\n");
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 400);
    }

    void bodyExceedingMaxBodyBytes_returns413() {
        m_server->setMaxBodyBytes(10);
        const QByteArray body(100, 'x');
        QByteArray raw = "POST /echo HTTP/1.1\r\nHost: x\r\n";
        raw += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        raw += body;

        const RawResponse resp = sendRawRequest(m_port, raw);
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 413);
    }

    void headersExceeding64KiB_returns431() {
        // Заголовки без конца (без \r\n\r\n) размером больше 64КБ — защита
        // от неограниченного роста буфера, см. HttpServer::tryProcess().
        QByteArray raw = "GET / HTTP/1.1\r\n";
        raw += QByteArray("X-Padding: ") + QByteArray(70 * 1024, 'a') + "\r\n";
        // Намеренно НЕ дописываем финальный \r\n\r\n — заголовки как бы
        // всё ещё "приходят".

        const RawResponse resp = sendRawRequest(m_port, raw);
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 431);
    }

    // addRoute(..., Handler) — асинхронный путь: respond() может быть
    // вызван из колбэка позже, а не сразу внутри самого addRoute-лямбда.
    // Именно так работает POST /jobs в main.cpp (после callAuthStub()).
    // Проверяем, что HttpServer действительно умеет ждать и корректно
    // сериализует ответ, пришедший asynchronously.
    void asyncHandler_respondsAfterDelay() {
        m_server->addRoute("GET", "/async", [](const HttpRequest &, HttpServer::RespondFn respond) {
            QTimer::singleShot(50, [respond]() {
                respond(HttpResponse::json(202, "Accepted", R"({"queued":true})"));
            });
        });

        const RawResponse resp = sendRawRequest(m_port, "GET /async HTTP/1.1\r\nHost: x\r\n\r\n");
        QVERIFY(resp.ok);
        QCOMPARE(resp.statusCode, 202);
        QCOMPARE(resp.body, QByteArray(R"({"queued":true})"));
    }

private:
    std::unique_ptr<HttpServer> m_server;
    quint16 m_port = 0;
};

QTEST_GUILESS_MAIN(TstHttpServer)
#include "tst_httpserver.moc"

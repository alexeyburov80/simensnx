#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSocketNotifier>
#include <QDebug>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "HttpServer.h"
#include "LicensePool.h"

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
        qInfo() << "[license-server-stub] shutdown signal received, stopping";
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

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8083").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[license-server-stub] invalid PORT env var";
        return 1;
    }

    bool seatsOk = false, ttlOk = false;
    const int seats = qEnvironmentVariable("LICENSE_SEATS", "3").toInt(&seatsOk);
    const int ttlSeconds = qEnvironmentVariable("LICENSE_TTL_SECONDS", "300").toInt(&ttlOk);

    LicensePool pool(seatsOk && seats > 0 ? seats : 3, ttlOk && ttlSeconds > 0 ? ttlSeconds : 300);

    HttpServer server;

    server.addRoute("GET", "/health", [&pool](const HttpRequest &) {
        pool.refresh();
        QJsonObject obj{
            {"status", "healthy"},
            {"seats_total", pool.totalSeats()},
            {"seats_in_use", pool.seatsInUse()},
        };
        return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.addRoute("GET", "/seats", [&pool](const HttpRequest &) {
        pool.refresh();
        QJsonObject obj{
            {"total", pool.totalSeats()},
            {"in_use", pool.seatsInUse()},
            {"available", pool.totalSeats() - pool.seatsInUse()},
            {"ttl_seconds", pool.ttlSeconds()},
        };
        return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.addRoute("POST", "/checkout", [&pool](const HttpRequest &req) -> HttpResponse {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(req.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return HttpResponse::json(400, "Bad Request", errorJson("invalid JSON body: " + parseError.errorString()));
        }
        const QJsonObject obj = doc.object();
        const QString clientId = obj.value("client_id").toString();
        const QString jobId = obj.value("job_id").toString();
        if (clientId.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("client_id is required"));
        }

        const LicensePool::CheckoutResult result = pool.checkout(clientId, jobId);
        if (!result.ok) {
            // Настоящий сигнал "нет свободных лицензий" — а не выдумка.
            // Клиент (nx-worker-stub) должен трактовать 503 как "верни
            // задачу в очередь / попробуй позже", а не как ошибку задачи.
            QJsonObject err{
                {"error", "no license seats available"},
                {"seats_total", pool.totalSeats()},
                {"seats_in_use", pool.seatsInUse()},
            };
            return HttpResponse::json(503, "Service Unavailable", QJsonDocument(err).toJson(QJsonDocument::Compact));
        }

        QJsonObject resp{
            {"token", result.token},
            {"expires_at", result.expiresAt.toString(Qt::ISODate)},
            {"ttl_seconds", pool.ttlSeconds()},
        };
        return HttpResponse::json(200, "OK", QJsonDocument(resp).toJson(QJsonDocument::Compact));
    });

    server.addRoute("POST", "/checkin", [&pool](const HttpRequest &req) -> HttpResponse {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(req.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return HttpResponse::json(400, "Bad Request", errorJson("invalid JSON body: " + parseError.errorString()));
        }
        const QString token = doc.object().value("token").toString();
        if (token.isEmpty()) {
            return HttpResponse::json(400, "Bad Request", errorJson("token is required"));
        }

        if (!pool.checkin(token)) {
            // Не считаем это server-side ошибкой: токен мог легитимно уже
            // истечь по TTL и быть освобождён sweep'ом раньше, чем клиент
            // успел сделать checkin. 404 — честный ответ, не 500.
            return HttpResponse::json(404, "Not Found", errorJson("token not found or already expired"));
        }
        return HttpResponse::json(200, "OK", R"({"status":"released"})");
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[license-server-stub] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[license-server-stub] listening on port" << port << "seats=" << pool.totalSeats()
            << "ttl=" << pool.ttlSeconds() << "s";

    return app.exec();
}

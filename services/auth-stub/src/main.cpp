#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QDebug>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "HttpServer.h"

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
        qInfo() << "[auth-stub] shutdown signal received, stopping";
        app.quit();
    });

    struct sigaction sa{};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    installSignalHandlers(app);

    bool portOk = false;
    const quint16 port = qEnvironmentVariable("PORT", "8081").toUShort(&portOk);
    if (!portOk) {
        qCritical() << "[auth-stub] invalid PORT env var";
        return 1;
    }

    HttpServer server;

    server.addRoute("GET", "/health", [](const HttpRequest &) {
        return HttpResponse::json(200, "OK", R"({"status":"healthy"})");
    });

    // Phase 0 по ROADMAP.md: "всегда пропускает запрос, но с реальным
    // сетевым вызовом (чтобы protocol/latency были видны сразу)". То есть
    // цель этой заглушки — не реальная проверка токена (её сознательно
    // нет), а чтобы api-server делал НАСТОЯЩИЙ HTTP round-trip перед каждой
    // публикацией задачи, и это было видно в задержках/логах уже сейчас,
    // а не только когда появится Keycloak/OAuth2 в Phase 3.
    server.addRoute("POST", "/verify", [](const HttpRequest &req) {
        QString clientId = "anonymous";
        if (req.headers.contains("authorization")) {
            const QByteArray token = req.headers.value("authorization");
            // Ничего не расшифровываем и не проверяем подпись — это стаб.
            // Берём короткий хэш токена просто как стабильный псевдо-id,
            // чтобы у ответа была хоть какая-то структура, похожая на то,
            // что вернёт настоящий auth-сервис в Phase 3.
            clientId = QString::fromUtf8(
                QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex().left(12));
        }

        QJsonObject obj{
            {"allowed", true},
            {"client_id", clientId},
            {"note", "auth-stub Phase 0: no real validation, always allows"},
        };
        return HttpResponse::json(200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    if (!server.listen(QHostAddress::AnyIPv4, port)) {
        qCritical() << "[auth-stub] failed to listen on port" << port;
        return 1;
    }
    qInfo() << "[auth-stub] listening on port" << port;

    return app.exec();
}

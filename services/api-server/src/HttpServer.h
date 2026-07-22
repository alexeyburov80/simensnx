#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QRegularExpression>
#include <functional>
#include <memory>
#include <vector>

#include "HttpTypes.h"

// Простой HTTP/1.1 сервер поверх QTcpServer/QTcpSocket.
//
// Сознательно не поддерживает keep-alive, chunked transfer-encoding и
// произвольные заголовки в ответе — это внутренний сервис за reverse-proxy /
// service mesh, а не публичный веб-сервер общего назначения. Каждое
// соединение обслуживает ровно один запрос и закрывается, что резко
// упрощает парсер и убирает целый класс багов "недочитанного тела" по
// сравнению с ручным substring-парсингом, который был в исходном коде.
class HttpServer : public QObject {
    Q_OBJECT
public:
    using RespondFn = std::function<void(HttpResponse)>;
    // Асинхронный обработчик: обязан вызвать respond() РОВНО ОДИН РАЗ,
    // сразу (для простых маршрутов) или позже, из колбэка QNetworkReply/
    // QSqlQuery и т.п. (для маршрутов, которым нужен внешний вызов —
    // например, POST /jobs теперь дергает auth-stub и Postgres перед
    // ответом). Синхронные маршруты не переписывались руками — см.
    // addRoute()/addRoutePattern() ниже, обёртка делает это автоматически.
    using Handler = std::function<void(const HttpRequest &, RespondFn respond)>;
    using SyncHandler = std::function<HttpResponse(const HttpRequest &)>;

    explicit HttpServer(QObject *parent = nullptr);

    // Регистрирует асинхронный обработчик для точного пути.
    void addRoute(const QByteArray &method, const QString &path, Handler handler);
    // Обёртка для старых синхронных обработчиков — respond() вызывается
    // немедленно тем же значением, что раньше возвращалось из функции.
    void addRoute(const QByteArray &method, const QString &path, SyncHandler handler);

    // То же для маршрутов с параметром вида "/jobs/{id}".
    void addRoutePattern(const QByteArray &method, const QRegularExpression &pattern, Handler handler);
    void addRoutePattern(const QByteArray &method, const QRegularExpression &pattern, SyncHandler handler);

    bool listen(const QHostAddress &address, quint16 port);

    // Реальный порт после listen() — отличается от переданного в listen(),
    // если был передан 0 (ОС сама выбирает свободный ephemeral-порт).
    // Нужно тестам, которые не хотят зависеть от фиксированного порта.
    quint16 port() const { return m_server.serverPort(); }

    // Верхняя граница Content-Length. Раньше подобной защиты не было нигде
    // в репозитории: любой сервис на cpp-httplib буферизовал тело целиком
    // без ограничений. Для файлового хранилища это особенно важно — не
    // ждём, пока придёт всё тело гигантской загрузки, отклоняем сразу по
    // заголовку.
    void setMaxBodyBytes(qint64 maxBytes) { m_maxBodyBytes = maxBytes; }

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    struct PatternRoute {
        QByteArray method;
        QRegularExpression pattern;
        Handler handler;
    };

    struct ConnectionState {
        QByteArray buffer;
        bool headersParsed = false;
        qint64 contentLength = 0;
        int headerEnd = -1;
        std::shared_ptr<bool> responded; // общий guard между dispatch() и таймаутом
    };

    void tryProcess(QTcpSocket *socket);
    void writeResponse(QTcpSocket *socket, const HttpResponse &resp);
    void dispatch(const HttpRequest &req, QTcpSocket *socket);

    QTcpServer m_server;
    qint64 m_maxBodyBytes = 100LL * 1024 * 1024; // 100 МБ по умолчанию
    QHash<QTcpSocket *, ConnectionState> m_connections;
    QHash<QString, QHash<QByteArray, Handler>> m_exactRoutes; // path -> method -> handler
    std::vector<PatternRoute> m_patternRoutes;
};

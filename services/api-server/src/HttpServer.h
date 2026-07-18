#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QRegularExpression>
#include <functional>
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
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    explicit HttpServer(QObject *parent = nullptr);

    // Регистрирует обработчик для точного пути ("/", "/health", ...).
    void addRoute(const QByteArray &method, const QString &path, Handler handler);

    // Регистрирует обработчик для пути с параметром вида "/jobs/{id}".
    // В HttpRequest::path параметр не подставляется — обработчик сам
    // получает его через захватывающую группу regexp (см. main.cpp).
    void addRoutePattern(const QByteArray &method, const QRegularExpression &pattern, Handler handler);

    bool listen(const QHostAddress &address, quint16 port);

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
    };

    void tryProcess(QTcpSocket *socket);
    void writeResponse(QTcpSocket *socket, const HttpResponse &resp);
    HttpResponse dispatch(const HttpRequest &req);

    QTcpServer m_server;
    QHash<QTcpSocket *, ConnectionState> m_connections;
    QHash<QString, QHash<QByteArray, Handler>> m_exactRoutes; // path -> method -> handler
    std::vector<PatternRoute> m_patternRoutes;
};

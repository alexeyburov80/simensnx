#include "HttpServer.h"

#include <QRegularExpressionMatch>

HttpServer::HttpServer(QObject *parent) : QObject(parent) {
    connect(&m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

void HttpServer::addRoute(const QByteArray &method, const QString &path, Handler handler) {
    m_exactRoutes[path][method] = std::move(handler);
}

void HttpServer::addRoutePattern(const QByteArray &method, const QRegularExpression &pattern, Handler handler) {
    m_patternRoutes.push_back({method, pattern, std::move(handler)});
}

bool HttpServer::listen(const QHostAddress &address, quint16 port) {
    return m_server.listen(address, port);
}

void HttpServer::onNewConnection() {
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        m_connections.insert(socket, ConnectionState{});
        connect(socket, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_connections.remove(socket);
            socket->deleteLater();
        });
    }
}

void HttpServer::onReadyRead() {
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket || !m_connections.contains(socket)) return;
    m_connections[socket].buffer.append(socket->readAll());
    tryProcess(socket);
}

void HttpServer::tryProcess(QTcpSocket *socket) {
    ConnectionState &st = m_connections[socket];

    if (!st.headersParsed) {
        st.headerEnd = st.buffer.indexOf("\r\n\r\n");
        if (st.headerEnd < 0) {
            // Заголовки ещё не пришли целиком. Защита от неограниченного
            // роста буфера на случай, если клиент шлёт мусор без конца.
            if (st.buffer.size() > 64 * 1024) {
                writeResponse(socket, HttpResponse::json(431, "Request Header Fields Too Large",
                                                           R"({"error":"headers too large"})"));
                socket->disconnectFromHost();
            }
            return;
        }
        st.headersParsed = true;
    }

    const QByteArray head = st.buffer.left(st.headerEnd);
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) {
        writeResponse(socket, HttpResponse::json(400, "Bad Request", R"({"error":"malformed request"})"));
        socket->disconnectFromHost();
        return;
    }

    const QByteArray requestLine = lines.first().trimmed();
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) {
        writeResponse(socket, HttpResponse::json(400, "Bad Request", R"({"error":"malformed request line"})"));
        socket->disconnectFromHost();
        return;
    }

    HttpRequest req;
    req.method = parts[0];
    QString rawPath = QString::fromUtf8(parts[1]);
    req.path = rawPath.section('?', 0, 0);

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        req.headers.insert(key, value);
    }

    st.contentLength = 0;
    if (req.headers.contains("content-length")) {
        bool ok = false;
        st.contentLength = req.headers.value("content-length").toLongLong(&ok);
        if (!ok || st.contentLength < 0) {
            writeResponse(socket, HttpResponse::json(400, "Bad Request", R"({"error":"invalid Content-Length"})"));
            socket->disconnectFromHost();
            return;
        }
        if (st.contentLength > m_maxBodyBytes) {
            writeResponse(socket, HttpResponse::json(413, "Payload Too Large",
                                                       R"({"error":"request body exceeds maximum allowed size"})"));
            socket->disconnectFromHost();
            return;
        }
    }

    const qint64 bodyStart = st.headerEnd + 4;
    const qint64 available = st.buffer.size() - bodyStart;
    if (available < st.contentLength) {
        return; // ждём остаток тела
    }

    req.body = st.buffer.mid(bodyStart, st.contentLength);

    const HttpResponse resp = dispatch(req);
    writeResponse(socket, resp);
    socket->disconnectFromHost();
}

HttpResponse HttpServer::dispatch(const HttpRequest &req) {
    if (m_exactRoutes.contains(req.path)) {
        const auto &methodMap = m_exactRoutes[req.path];
        if (methodMap.contains(req.method)) {
            return methodMap[req.method](req);
        }
        return HttpResponse::json(405, "Method Not Allowed", R"({"error":"method not allowed"})");
    }

    for (const auto &route : m_patternRoutes) {
        if (route.method != req.method) continue;
        QRegularExpressionMatch m = route.pattern.match(req.path);
        if (m.hasMatch()) {
            HttpRequest withParams = req;
            for (int i = 1; i <= m.lastCapturedIndex(); ++i) {
                withParams.pathParams << m.captured(i);
            }
            return route.handler(withParams);
        }
    }

    return HttpResponse::json(404, "Not Found", R"({"error":"not found"})");
}

void HttpServer::writeResponse(QTcpSocket *socket, const HttpResponse &resp) {
    QByteArray out;
    out += "HTTP/1.1 " + QByteArray::number(resp.statusCode) + " " + resp.statusText + "\r\n";
    out += "Content-Type: " + resp.contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(resp.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "\r\n";
    out += resp.body;
    socket->write(out);
    socket->flush();
}

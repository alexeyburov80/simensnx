#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

// Минимальный, но корректный набор структур для HTTP/1.1 запроса и ответа.
// Никаких внешних HTTP-библиотек (httplib и т.п.) — только Qt Network.
struct HttpRequest {
    QByteArray method;                 // "GET", "POST", ...
    QString path;                      // без query string
    QHash<QByteArray, QByteArray> headers; // ключи в нижнем регистре
    QByteArray body;
    QStringList pathParams;            // захваченные группы regexp-маршрута
};

struct HttpResponse {
    int statusCode = 200;
    QByteArray statusText = "OK";
    QByteArray contentType = "application/json";
    QByteArray body;

    static HttpResponse json(int code, const QByteArray &statusText, const QByteArray &jsonBody) {
        HttpResponse r;
        r.statusCode = code;
        r.statusText = statusText;
        r.body = jsonBody;
        return r;
    }
};

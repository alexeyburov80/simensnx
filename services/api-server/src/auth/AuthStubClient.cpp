#include "AuthStubClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

AuthStubClient::AuthStubClient(QNetworkAccessManager &nam, QUrl authServiceUrl)
    : m_nam(nam), m_authServiceUrl(std::move(authServiceUrl))
{
}

void AuthStubClient::verify(const QByteArray &authHeader, std::function<void(bool, QString)> callback) const
{
    QUrl url = m_authServiceUrl;
    url.setPath(url.path() + "/verify");

    QNetworkRequest request(url);
    if (!authHeader.isEmpty()) {
        request.setRawHeader("Authorization", authHeader);
    }

    auto *reply = m_nam.post(request, QByteArray());
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(3000);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            reply->deleteLater();
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!doc.isObject() || !doc.object().value("allowed").toBool(false)) {
            callback(false, "auth-stub denied the request");
            return;
        }
        callback(true, {});
    });
}

#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

// Раньше была свободная функция callAuthStub() внутри анонимного
// namespace в main.cpp — вынесена в класс, чтобы JobsController зависел
// от неё через конструктор (DI), а не от свободной функции, замкнутой на
// глобальный AppConfig. Логика не изменилась: реальный HTTP POST /verify
// с таймаутом 3с, см. ROADMAP.md Phase 0 про auth-stub.
class AuthStubClient {
public:
    AuthStubClient(QNetworkAccessManager &nam, QUrl authServiceUrl);

    // Возвращает false и сообщение об ошибке, если auth-stub недоступен
    // или ответил не allowed:true — вызывающий код обязан трактовать это
    // как fail closed (не публиковать задачу), а не как "ну и ладно".
    void verify(const QByteArray &authHeader, std::function<void(bool allowed, QString error)> callback) const;

private:
    QNetworkAccessManager &m_nam;
    QUrl m_authServiceUrl;
};

#pragma once

#include <QString>
#include <QByteArray>

// Абстракция над "опубликовать сообщение в AMQP-обменник/очередь".
// Единственная причина существования этого интерфейса — тестируемость:
// JobsController (services/api-server/src/jobs) должен проверяться QTest
// без amqpcpp, который сознательно исключён из api-server-core (см.
// services/api-server/CMakeLists.txt, API_SERVER_BUILD_APP) — сборка с
// amqpcpp заметно дольше, и тестам сама библиотека не нужна, нужен только
// факт вызова с правильными аргументами.
//
// QtAmqpConnection (services/api-server/src/QtAmqpConnection.h) реализует
// этот интерфейс напрямую — его существующие publish()/isReady() уже имеют
// ровно такую сигнатуру, изменений в логике не потребовалось. Тесты
// используют лёгкий in-memory фейк вместо реального подключения к брокеру.
class IJobPublisher {
public:
    virtual ~IJobPublisher() = default;

    virtual bool publish(const QString &exchange, const QString &routingKey, const QByteArray &body) = 0;

    virtual bool isReady() const = 0;
};

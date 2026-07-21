#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <unordered_map>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>

// Та же схема интеграции AMQP-CPP с Qt event loop'ом, что и в api-server
// (см. services/api-server/src/QtAmqpConnection.{h,cpp}) — там подробно
// объяснено, почему это QSocketNotifier, а не reinterpret_cast и не libev.
//
// Это осознанное дублирование ~60 строк "сантехники" monitor()/onConnected()
// между двумя сервисами, а не общая библиотека: на этом этапе сервисы
// эволюционируют независимо, и вынесение в libs/nx-amqp-qt — отдельное
// решение, которое имеет смысл принимать, когда таких сервисов станет
// больше двух, и трогать уже выпущенный api-server ради этого сейчас
// избыточно.
//
// В отличие от api-server, этот класс — консьюмер: подписывается на очередь
// и вызывает callback на каждое сообщение, а подтверждение (ack/reject)
// делает вызывающий код, когда обработка реально завершена.
class QtAmqpConsumer : public QObject, public AMQP::TcpHandler {
    Q_OBJECT
public:
    using MessageHandler = std::function<void(const AMQP::Message &message, uint64_t deliveryTag, bool redelivered)>;

    explicit QtAmqpConsumer(QObject *parent = nullptr);
    ~QtAmqpConsumer() override;

    void connectToServer(const QUrl &rabbitmqUrl);

    bool isReady() const { return m_ready; }

    // prefetchCount ограничивает число неподтверждённых сообщений на канал —
    // без этого RabbitMQ отдаёт консьюмеру все сообщения из очереди разом,
    // что для тяжёлых задач NX означает наливать в память работу на часы
    // вперёд вместо честного round-robin между воркерами.
    void consume(const QString &queue, int prefetchCount, MessageHandler handler);

    // Вариант для подписки не на именованную рабочую очередь, а на копию
    // событий из fanout-обменника: объявляет обменник, объявляет свою
    // durable-очередь с заданным именем и биндит её к обменнику, затем
    // consume как обычно. Нужен job-orchestrator'у, чтобы получать копию
    // каждого сообщения о создании задачи, не забирая её из jobs.process/
    // jobs.validate, которые продолжает потреблять nx-worker-stub.
    void consumeFromFanoutExchange(const QString &exchange, const QString &queueName,
                                    int prefetchCount, MessageHandler handler);

    void ack(uint64_t deliveryTag);
    void reject(uint64_t deliveryTag, bool requeue);

    // Публикация — нужна для retry-цикла: при повторной попытке
    // job-orchestrator сам кладёт задачу обратно в jobs.process/jobs.validate,
    // тем же каналом, которым consume'ится работа (AMQP это разрешает).
    bool publish(const QString &exchange, const QString &routingKey, const QByteArray &body);

    // Объявляет jobs.process/jobs.validate с ТЕМИ ЖЕ аргументами, что и в
    // api-server/nx-worker-stub (durable, без дополнительных Table-аргументов).
    // Обязательно должно совпадать побитово — иначе RabbitMQ рвёт канал
    // PRECONDITION_FAILED (см. docs/ADDING_A_SERVICE.md).
    void declareWorkQueues();

signals:
    void ready();
    void connectionError(QString message);
    void connectionClosed();

protected:
    void monitor(AMQP::TcpConnection *connection, int fd, int flags) override;
    void onError(AMQP::TcpConnection *connection, const char *message) override;
    void onConnected(AMQP::TcpConnection *connection) override;
    void onClosed(AMQP::TcpConnection *connection) override;

    // Только согласовывает интервал с брокером — реальную отправку делает
    // m_heartbeatTimer, см. подробный комментарий в
    // services/api-server/src/QtAmqpConnection.h (там же найден и починен
    // этот баг: RabbitMQ рвал соединение по "missed heartbeats", потому что
    // AMQP-CPP не отправляет heartbeat-фреймы сама по себе).
    uint16_t onNegotiate(AMQP::TcpConnection *connection, uint16_t interval) override;

private:
    struct FdWatch {
        std::unique_ptr<QSocketNotifier> read;
        std::unique_ptr<QSocketNotifier> write;
    };

    void onFdActivated(int fd, int flag);
    void doConsume(const std::string &queue, int prefetchCount, MessageHandler handler);
    void sendHeartbeat();
    void scheduleReconnect();
    void doConnect();

    std::unique_ptr<AMQP::TcpConnection> m_connection;
    std::unique_ptr<AMQP::TcpChannel> m_channel;
    std::unordered_map<int, FdWatch> m_watches;
    bool m_ready = false;
    QTimer m_heartbeatTimer;
    uint16_t m_negotiatedHeartbeat = 0;

    // См. подробный комментарий в services/api-server/src/QtAmqpConnection.h —
    // без автопереподключения разорванное соединение оставалось бы мёртвым
    // навсегда, и job-orchestrator переставал бы видеть новые задачи и
    // события завершения до ручного рестарта.
    QUrl m_url;
    QTimer m_reconnectTimer;
    int m_reconnectDelayMs = 2000;
    static constexpr int kMaxReconnectDelayMs = 30000;
};

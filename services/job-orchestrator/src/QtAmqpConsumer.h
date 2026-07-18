#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
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

signals:
    void ready();
    void connectionError(QString message);
    void connectionClosed();

protected:
    void monitor(AMQP::TcpConnection *connection, int fd, int flags) override;
    void onError(AMQP::TcpConnection *connection, const char *message) override;
    void onConnected(AMQP::TcpConnection *connection) override;
    void onClosed(AMQP::TcpConnection *connection) override;

private:
    struct FdWatch {
        std::unique_ptr<QSocketNotifier> read;
        std::unique_ptr<QSocketNotifier> write;
    };

    void onFdActivated(int fd, int flag);
    void doConsume(const std::string &queue, int prefetchCount, MessageHandler handler);

    std::unique_ptr<AMQP::TcpConnection> m_connection;
    std::unique_ptr<AMQP::TcpChannel> m_channel;
    std::unordered_map<int, FdWatch> m_watches;
    bool m_ready = false;
};

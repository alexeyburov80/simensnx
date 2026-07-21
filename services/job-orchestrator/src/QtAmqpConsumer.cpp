#include "QtAmqpConsumer.h"

#include <algorithm>
#include <QDebug>

QtAmqpConsumer::QtAmqpConsumer(QObject *parent) : QObject(parent) {
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &QtAmqpConsumer::sendHeartbeat);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &QtAmqpConsumer::doConnect);
}

QtAmqpConsumer::~QtAmqpConsumer() {
    m_heartbeatTimer.stop();
    m_reconnectTimer.stop();
    if (m_channel) m_channel->close();
    if (m_connection) m_connection->close();
}

void QtAmqpConsumer::connectToServer(const QUrl &rabbitmqUrl) {
    m_url = rabbitmqUrl;
    doConnect();
}

void QtAmqpConsumer::doConnect() {
    const std::string host = m_url.host().toStdString();
    const quint16 port = m_url.port(5672);
    const std::string user = m_url.userName().toStdString();
    const std::string pass = m_url.password().toStdString();
    std::string vhost = m_url.path().toStdString();
    if (vhost.empty()) vhost = "/";

    m_channel.reset();

    AMQP::Address address(host, port, AMQP::Login(user, pass), vhost);
    m_connection = std::make_unique<AMQP::TcpConnection>(this, address);
    qInfo() << "[AMQP] connecting to" << m_url.host() << ":" << port;
}

void QtAmqpConsumer::scheduleReconnect() {
    if (m_reconnectTimer.isActive()) return;
    qWarning() << "[AMQP] reconnecting in" << m_reconnectDelayMs << "ms";
    m_reconnectTimer.start(m_reconnectDelayMs);
    m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
}

uint16_t QtAmqpConsumer::onNegotiate(AMQP::TcpConnection * /*connection*/, uint16_t interval) {
    m_negotiatedHeartbeat = interval;
    qInfo() << "[AMQP] heartbeat interval negotiated:" << interval << "s";
    return interval;
}

void QtAmqpConsumer::sendHeartbeat() {
    if (m_connection) m_connection->heartbeat();
}

void QtAmqpConsumer::monitor(AMQP::TcpConnection * /*connection*/, int fd, int flags) {
    if (flags == 0) {
        m_watches.erase(fd);
        return;
    }

    FdWatch &watch = m_watches[fd];
    const bool wantRead = flags & AMQP::readable;
    const bool wantWrite = flags & AMQP::writable;

    if (wantRead && !watch.read) {
        watch.read = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read, this);
        connect(watch.read.get(), &QSocketNotifier::activated, this,
                [this, fd](QSocketDescriptor, QSocketNotifier::Type) { onFdActivated(fd, AMQP::readable); });
    } else if (!wantRead && watch.read) {
        watch.read.reset();
    }

    if (wantWrite && !watch.write) {
        watch.write = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Write, this);
        connect(watch.write.get(), &QSocketNotifier::activated, this,
                [this, fd](QSocketDescriptor, QSocketNotifier::Type) { onFdActivated(fd, AMQP::writable); });
    } else if (!wantWrite && watch.write) {
        watch.write.reset();
    }
}

void QtAmqpConsumer::onFdActivated(int fd, int flag) {
    if (!m_connection) return;
    m_connection->process(fd, flag);
}

void QtAmqpConsumer::onError(AMQP::TcpConnection * /*connection*/, const char *message) {
    m_ready = false;
    m_heartbeatTimer.stop();
    qWarning() << "[AMQP] connection error:" << message;
    emit connectionError(QString::fromUtf8(message));
    scheduleReconnect();
}

void QtAmqpConsumer::onConnected(AMQP::TcpConnection *connection) {
    qInfo() << "[AMQP] TCP connection established, opening channel";
    m_reconnectDelayMs = 2000;
    m_channel = std::make_unique<AMQP::TcpChannel>(connection);

    m_channel->onError([this](const char *message) {
        m_ready = false;
        qWarning() << "[AMQP] channel error:" << message;
        emit connectionError(QString::fromUtf8(message));
    });

    m_channel->onReady([this]() {
        m_ready = true;
        qInfo() << "[AMQP] channel ready";
        emit ready();
    });

    if (m_negotiatedHeartbeat > 0) {
        m_heartbeatTimer.start(m_negotiatedHeartbeat * 500); // interval/2 в мс
        qInfo() << "[AMQP] sending heartbeats every" << (m_negotiatedHeartbeat / 2) << "s";
    }
}

void QtAmqpConsumer::onClosed(AMQP::TcpConnection * /*connection*/) {
    m_ready = false;
    m_heartbeatTimer.stop();
    qInfo() << "[AMQP] connection closed";
    emit connectionClosed();
    scheduleReconnect();
}

void QtAmqpConsumer::consume(const QString &queue, int prefetchCount, MessageHandler handler) {
    if (!m_channel) {
        qWarning() << "[AMQP] consume() called before channel is ready, ignoring for queue" << queue;
        return;
    }
    const std::string q = queue.toStdString();
    m_channel->declareQueue(q, AMQP::durable);
    doConsume(q, prefetchCount, std::move(handler));
}

void QtAmqpConsumer::consumeFromFanoutExchange(const QString &exchange, const QString &queueName,
                                                int prefetchCount, MessageHandler handler) {
    if (!m_channel) {
        qWarning() << "[AMQP] consumeFromFanoutExchange() called before channel is ready, ignoring" << exchange;
        return;
    }
    const std::string ex = exchange.toStdString();
    const std::string q = queueName.toStdString();

    // declareExchange идемпотентен (RabbitMQ не жалуется на повторное
    // объявление с теми же параметрами), поэтому неважно, что api-server
    // уже объявил этот же обменник — какой из сервисов запустится раньше,
    // не имеет значения.
    m_channel->declareExchange(ex, AMQP::fanout, AMQP::durable);
    m_channel->declareQueue(q, AMQP::durable);
    m_channel->bindQueue(ex, q, "");

    doConsume(q, prefetchCount, std::move(handler));
}

void QtAmqpConsumer::doConsume(const std::string &queue, int prefetchCount, MessageHandler handler) {
    // QoS выставляется per-channel в AMQP-CPP, а не per-consumer — если в
    // будущем на один канал повесят несколько consume() с разным prefetch,
    // это будет молча неверно. Пока у нас один-два consume на канал с
    // одинаковым prefetch, это не проблема, но стоит держать в голове.
    m_channel->setQos(prefetchCount);

    m_channel->consume(queue)
        .onReceived([handler](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
            handler(message, deliveryTag, redelivered);
        })
        .onError([queue](const char *message) {
            qWarning() << "[AMQP] consume error on queue" << QString::fromStdString(queue) << ":" << message;
        });

    qInfo() << "[AMQP] consuming from queue" << QString::fromStdString(queue) << "with prefetch" << prefetchCount;
}

void QtAmqpConsumer::ack(uint64_t deliveryTag) {
    if (m_channel) m_channel->ack(deliveryTag);
}

void QtAmqpConsumer::reject(uint64_t deliveryTag, bool requeue) {
    if (m_channel) m_channel->reject(deliveryTag, requeue ? AMQP::requeue : 0);
}

bool QtAmqpConsumer::publish(const QString &exchange, const QString &routingKey, const QByteArray &body) {
    if (!m_ready || !m_channel) return false;
    // persistent=true — см. подробный комментарий в
    // services/api-server/src/QtAmqpConnection.cpp: durable-очередь без
    // этого флага переживёт рестарт брокера, а сообщения внутри — нет.
    AMQP::Envelope envelope(body.constData(), body.size());
    envelope.setPersistent(true);
    return m_channel->publish(exchange.toStdString(), routingKey.toStdString(), envelope);
}

void QtAmqpConsumer::declareWorkQueues() {
    if (!m_channel) return;
    m_channel->declareQueue("jobs.process", AMQP::durable);
    m_channel->declareQueue("jobs.validate", AMQP::durable);
}

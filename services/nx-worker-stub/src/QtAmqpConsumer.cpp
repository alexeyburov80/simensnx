#include "QtAmqpConsumer.h"

#include <QDebug>

QtAmqpConsumer::QtAmqpConsumer(QObject *parent) : QObject(parent) {}

QtAmqpConsumer::~QtAmqpConsumer() {
    if (m_channel) m_channel->close();
    if (m_connection) m_connection->close();
}

void QtAmqpConsumer::connectToServer(const QUrl &rabbitmqUrl) {
    const std::string host = rabbitmqUrl.host().toStdString();
    const quint16 port = rabbitmqUrl.port(5672);
    const std::string user = rabbitmqUrl.userName().toStdString();
    const std::string pass = rabbitmqUrl.password().toStdString();
    std::string vhost = rabbitmqUrl.path().toStdString();
    if (vhost.empty()) vhost = "/";

    AMQP::Address address(host, port, AMQP::Login(user, pass), vhost);
    m_connection = std::make_unique<AMQP::TcpConnection>(this, address);
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
    qWarning() << "[AMQP] connection error:" << message;
    emit connectionError(QString::fromUtf8(message));
}

void QtAmqpConsumer::onConnected(AMQP::TcpConnection *connection) {
    qInfo() << "[AMQP] TCP connection established, opening channel";
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
}

void QtAmqpConsumer::onClosed(AMQP::TcpConnection * /*connection*/) {
    m_ready = false;
    qInfo() << "[AMQP] connection closed";
    emit connectionClosed();
}

void QtAmqpConsumer::consume(const QString &queue, int prefetchCount, MessageHandler handler) {
    if (!m_channel) {
        qWarning() << "[AMQP] consume() called before channel is ready, ignoring for queue" << queue;
        return;
    }

    const std::string q = queue.toStdString();

    // QoS выставляется per-channel в AMQP-CPP, а не per-consumer — если в
    // будущем на один канал повесят несколько consume() с разным prefetch,
    // это будет молча неверно. Пока у нас один consume на канал, это не
    // проблема, но стоит держать в голове при расширении.
    m_channel->setQos(prefetchCount);

    m_channel->declareQueue(q, AMQP::durable);

    m_channel->consume(q)
        .onReceived([handler](const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
            handler(message, deliveryTag, redelivered);
        })
        .onError([queue](const char *message) {
            qWarning() << "[AMQP] consume error on queue" << queue << ":" << message;
        });

    qInfo() << "[AMQP] consuming from queue" << queue << "with prefetch" << prefetchCount;
}

void QtAmqpConsumer::ack(uint64_t deliveryTag) {
    if (m_channel) m_channel->ack(deliveryTag);
}

void QtAmqpConsumer::reject(uint64_t deliveryTag, bool requeue) {
    if (m_channel) m_channel->reject(deliveryTag, requeue ? AMQP::requeue : 0);
}

void QtAmqpConsumer::declareCompletionExchange(const QString &exchange) {
    if (!m_channel) {
        qWarning() << "[AMQP] declareCompletionExchange() called before channel is ready";
        return;
    }
    m_channel->declareExchange(exchange.toStdString(), AMQP::fanout, AMQP::durable);
}

bool QtAmqpConsumer::publish(const QString &exchange, const QString &routingKey, const QByteArray &body) {
    if (!m_ready || !m_channel) return false;
    return m_channel->publish(exchange.toStdString(), routingKey.toStdString(), body.constData(), body.size());
}

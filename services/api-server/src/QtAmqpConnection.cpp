#include "QtAmqpConnection.h"

#include <QDebug>

QtAmqpConnection::QtAmqpConnection(QObject *parent) : QObject(parent) {}

QtAmqpConnection::~QtAmqpConnection() {
    // Notifier'ы должны быть уничтожены до закрытия сокетов, unique_ptr в
    // m_watches и порядок членов это гарантируют. Явно закрываем канал/
    // соединение, чтобы RabbitMQ увидел штатное закрытие, а не обрыв TCP.
    if (m_channel) m_channel->close();
    if (m_connection) m_connection->close();
}

void QtAmqpConnection::connectToServer(const QUrl &rabbitmqUrl) {
    const std::string host = rabbitmqUrl.host().toStdString();
    const quint16 port = rabbitmqUrl.port(5672);
    const std::string user = rabbitmqUrl.userName().toStdString();
    const std::string pass = rabbitmqUrl.password().toStdString();
    std::string vhost = rabbitmqUrl.path().toStdString();
    if (vhost.empty()) vhost = "/";

    AMQP::Address address(host, port, AMQP::Login(user, pass), vhost);
    m_connection = std::make_unique<AMQP::TcpConnection>(this, address);
}
/**
 * @brief QtAmqpConnection::onNegotiate Переопределен метод heartbeat-интервал в секундах. Меньше таймаута RabbitMQ (60 секунд)
 *
 * Если нет данных 20 секунд → отправляется heartbeat (1 байт)
 * Если снова нет данных 20 секунд → опять heartbeat
 * И так пока есть соединение
 *
 * @param connection
 * @param interval
 * @return заменить для продакшена 30-60 секунд
 */
uint16_t QtAmqpConnection::onNegotiate(AMQP::TcpConnection *connection, uint16_t interval) {
    return 20;
}

void QtAmqpConnection::monitor(AMQP::TcpConnection * /*connection*/, int fd, int flags) {
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

void QtAmqpConnection::onFdActivated(int fd, int flag) {
    if (!m_connection) return;
    // process() может внутри себя вызвать monitor() и поменять/удалить набор
    // fd — именно поэтому watch берётся по значению fd, а не по указателю.
    m_connection->process(fd, flag);
}

void QtAmqpConnection::onError(AMQP::TcpConnection * /*connection*/, const char *message) {
    m_ready = false;
    qWarning() << "[AMQP] connection error:" << message;
    emit connectionError(QString::fromUtf8(message));
}

void QtAmqpConnection::onConnected(AMQP::TcpConnection *connection) {
    qInfo() << "[AMQP] TCP connection established, opening channel";
    m_channel = std::make_unique<AMQP::TcpChannel>(connection);

    m_channel->onError([this](const char *message) {
        m_ready = false;
        qWarning() << "[AMQP] channel error:" << message;
        emit connectionError(QString::fromUtf8(message));
    });

    m_channel->onReady([this]() {
        m_channel->declareQueue("jobs.process", AMQP::durable);
        m_channel->declareQueue("jobs.validate", AMQP::durable);
        m_channel->declareQueue("jobs.dlq", AMQP::durable);

        // Отдельный fanout-обменник для "событий о создании задачи" —
        // job-orchestrator подписан на него своей очередью и пишет
        // состояние в PostgreSQL (см. services/job-orchestrator).
        // Существующие jobs.process/jobs.validate трогать не пришлось:
        // nx-worker-stub как забирал работу оттуда напрямую, так и
        // продолжает, ничего в его поведении не поменялось.
        m_channel->declareExchange("jobs.events", AMQP::fanout, AMQP::durable);

        m_ready = true;
        qInfo() << "[AMQP] channel ready, queues declared";
        emit ready();
    });
}

void QtAmqpConnection::onClosed(AMQP::TcpConnection * /*connection*/) {
    m_ready = false;
    qInfo() << "[AMQP] connection closed";
    emit connectionClosed();
}

bool QtAmqpConnection::publish(const QString &exchange, const QString &routingKey, const QByteArray &body) {
    if (!m_ready || !m_channel) return false;
    return m_channel->publish(exchange.toStdString(), routingKey.toStdString(), body.constData(), body.size());
}

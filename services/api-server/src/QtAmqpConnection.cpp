#include "QtAmqpConnection.h"

#include <algorithm>
#include <QDebug>

QtAmqpConnection::QtAmqpConnection(QObject *parent) : QObject(parent) {
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &QtAmqpConnection::sendHeartbeat);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &QtAmqpConnection::doConnect);
}

QtAmqpConnection::~QtAmqpConnection() {
    m_heartbeatTimer.stop();
    m_reconnectTimer.stop();
    // Notifier'ы должны быть уничтожены до закрытия сокетов, unique_ptr в
    // m_watches и порядок членов это гарантируют. Явно закрываем канал/
    // соединение, чтобы RabbitMQ увидел штатное закрытие, а не обрыв TCP.
    if (m_channel) m_channel->close();
    if (m_connection) m_connection->close();
}

void QtAmqpConnection::connectToServer(const QUrl &rabbitmqUrl) {
    m_url = rabbitmqUrl;
    doConnect();
}

void QtAmqpConnection::doConnect() {
    const std::string host = m_url.host().toStdString();
    const quint16 port = m_url.port(5672);
    const std::string user = m_url.userName().toStdString();
    const std::string pass = m_url.password().toStdString();
    std::string vhost = m_url.path().toStdString();
    if (vhost.empty()) vhost = "/";

    // Явно роняем старый канал перед пересозданием соединения — иначе он
    // указывал бы на уже уничтоженный AMQP::TcpConnection после того, как
    // m_connection ниже будет переприсвоен.
    m_channel.reset();

    AMQP::Address address(host, port, AMQP::Login(user, pass), vhost);
    m_connection = std::make_unique<AMQP::TcpConnection>(this, address);
    qInfo() << "[AMQP] connecting to" << m_url.host() << ":" << port;
}

void QtAmqpConnection::scheduleReconnect() {
    if (m_reconnectTimer.isActive()) return; // уже запланировано этим же разрывом
    qWarning() << "[AMQP] reconnecting in" << m_reconnectDelayMs << "ms";
    m_reconnectTimer.start(m_reconnectDelayMs);
    m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
}

uint16_t QtAmqpConnection::onNegotiate(AMQP::TcpConnection * /*connection*/, uint16_t interval) {
    m_negotiatedHeartbeat = interval;
    qInfo() << "[AMQP] heartbeat interval negotiated:" << interval << "s";
    return interval; // соглашаемся с предложением брокера, не выдумываем своё
}

void QtAmqpConnection::sendHeartbeat() {
    if (m_connection) m_connection->heartbeat();
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
    m_heartbeatTimer.stop();
    qWarning() << "[AMQP] connection error:" << message;
    emit connectionError(QString::fromUtf8(message));
    scheduleReconnect();
}

void QtAmqpConnection::onConnected(AMQP::TcpConnection *connection) {
    qInfo() << "[AMQP] TCP connection established, opening channel";
    m_reconnectDelayMs = 2000; // успешный коннект — сбрасываем бэкофф до стартового значения
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

    // interval == 0 означает, что брокер heartbeat отключил (или клиент
    // предложил 0) — тогда таймер не нужен вовсе. Половина интервала — с
    // запасом на джиттер event loop'а/сети, чтобы фрейм гарантированно
    // дошёл до того, как брокер сочтёт соединение мёртвым.
    if (m_negotiatedHeartbeat > 0) {
        m_heartbeatTimer.start(m_negotiatedHeartbeat * 500); // interval/2 в мс
        qInfo() << "[AMQP] sending heartbeats every" << (m_negotiatedHeartbeat / 2) << "s";
    }
}

void QtAmqpConnection::onClosed(AMQP::TcpConnection * /*connection*/) {
    m_ready = false;
    m_heartbeatTimer.stop();
    qInfo() << "[AMQP] connection closed";
    emit connectionClosed();
    scheduleReconnect();
}

bool QtAmqpConnection::publish(const QString &exchange, const QString &routingKey, const QByteArray &body) {
    if (!m_ready || !m_channel) return false;
    return m_channel->publish(exchange.toStdString(), routingKey.toStdString(), body.constData(), body.size());
}

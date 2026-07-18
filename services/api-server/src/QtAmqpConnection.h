#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QUrl>
#include <memory>
#include <unordered_map>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>

// Интеграция AMQP-CPP с event loop'ом Qt.
//
// AMQP-CPP не выполняет сетевой ввод-вывод сам — он вызывает monitor(fd, flags),
// когда ему нужно узнавать о готовности файлового дескриптора к чтению/записи,
// и ждёт, что embedder вызовет connection->process(fd, flags), когда дескриптор
// готов. Официальные примеры делают это через libev/libuv/libevent; здесь то же
// самое сделано через QSocketNotifier, то есть без каких-либо сторонних
// event-loop библиотек — только Qt.
class QtAmqpConnection : public QObject, public AMQP::TcpHandler {
    Q_OBJECT
public:
    explicit QtAmqpConnection(QObject *parent = nullptr);
    ~QtAmqpConnection() override;

    // rabbitmqUrl вида amqp://user:pass@host:port/vhost
    void connectToServer(const QUrl &rabbitmqUrl);

    // true только после успешного onReady() (соединение установлено И канал открыт).
    bool isReady() const { return m_ready; }

    // Возвращает false, если канал ещё не готов — вызывающий код должен
    // трактовать это как временную недоступность брокера (503), а не как
    // "сообщение отправлено".
    bool publish(const QString &exchange, const QString &routingKey, const QByteArray &body);
    uint16_t onNegotiate(AMQP::TcpConnection *connection, uint16_t interval) override;

signals:
    void ready();
    void connectionError(QString message);
    void connectionClosed();

protected:
    // --- AMQP::TcpHandler ---
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

    // AMQP::TcpConnection НЕ наследуется от AMQP::Connection в этой версии
    // библиотеки (это и стало причиной reinterpret_cast в исходном коде).
    // Официальный способ получить канал поверх TcpConnection — специальный
    // класс AMQP::TcpChannel, ради этого и существующий.
    std::unique_ptr<AMQP::TcpConnection> m_connection;
    std::unique_ptr<AMQP::TcpChannel> m_channel;
    std::unordered_map<int, FdWatch> m_watches;
    bool m_ready = false;
};

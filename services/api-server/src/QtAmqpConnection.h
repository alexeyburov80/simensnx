#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <unordered_map>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>

#include "JobPublisher.h"

// Интеграция AMQP-CPP с event loop'ом Qt.
//
// AMQP-CPP не выполняет сетевой ввод-вывод сам — он вызывает monitor(fd, flags),
// когда ему нужно узнавать о готовности файлового дескриптора к чтению/записи,
// и ждёт, что embedder вызовет connection->process(fd, flags), когда дескриптор
// готов. Официальные примеры делают это через libev/libuv/libevent; здесь то же
// самое сделано через QSocketNotifier, то есть без каких-либо сторонних
// event-loop библиотек — только Qt.
//
// Реализует IJobPublisher (common/JobPublisher.h) — publish()/isReady() уже
// имели ровно нужную сигнатуру, изменений в логике не потребовалось. Это
// то, что позволяет JobsController зависеть от абстракции, а не от
// amqpcpp напрямую (DIP) — и, как следствие, тестироваться без него.
class QtAmqpConnection : public QObject, public AMQP::TcpHandler, public IJobPublisher {
    Q_OBJECT
public:
    explicit QtAmqpConnection(QObject *parent = nullptr);
    ~QtAmqpConnection() override;

    // rabbitmqUrl вида amqp://user:pass@host:port/vhost
    void connectToServer(const QUrl &rabbitmqUrl);

    // true только после успешного onReady() (соединение установлено И канал открыт).
    bool isReady() const override { return m_ready; }

    // Возвращает false, если канал ещё не готов — вызывающий код должен
    // трактовать это как временную недоступность брокера (503), а не как
    // "сообщение отправлено".
    bool publish(const QString &exchange, const QString &routingKey, const QByteArray &body) override;

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

    // ВАЖНО: этот callback только СОГЛАСОВЫВАЕТ интервал с брокером — сам по
    // себе он не заставляет AMQP-CPP отправлять heartbeat-фреймы. Библиотека
    // не делает это автоматически ни при каком значении, возвращённом
    // отсюда; реальную отправку embedder обязан делать сам через
    // connection->heartbeat() (см. m_heartbeatTimer ниже). Оставляем
    // предложение сервера как есть (RabbitMQ по умолчанию — 60с), а не
    // произвольно укорачиваем его: без реальной отправки укорачивание
    // интервала только приближает разрыв, не устраняя причину.
    uint16_t onNegotiate(AMQP::TcpConnection *connection, uint16_t interval) override;

private:
    struct FdWatch {
        std::unique_ptr<QSocketNotifier> read;
        std::unique_ptr<QSocketNotifier> write;
    };

    void onFdActivated(int fd, int flag);
    void sendHeartbeat();
    void scheduleReconnect();
    void doConnect();

    // AMQP::TcpConnection НЕ наследуется от AMQP::Connection в этой версии
    // библиотеки (это и стало причиной reinterpret_cast в исходном коде).
    // Официальный способ получить канал поверх TcpConnection — специальный
    // класс AMQP::TcpChannel, ради этого и существующий.
    std::unique_ptr<AMQP::TcpConnection> m_connection;
    std::unique_ptr<AMQP::TcpChannel> m_channel;
    std::unordered_map<int, FdWatch> m_watches;
    bool m_ready = false;

    // Отправляет heartbeat на брокер на середине согласованного интервала
    // (стандартная практика: запас на джиттер планировщика/сети, чтобы
    // фрейм гарантированно пришёл раньше, чем брокер сочтёт соединение
    // мёртвым). Без этого таймера RabbitMQ рвёт соединение по таймауту
    // "missed heartbeats" ровно так, как было до этого фикса.
    QTimer m_heartbeatTimer;
    uint16_t m_negotiatedHeartbeat = 0;

    // Автопереподключение. Никакой heartbeat не спасёт от разрыва, если
    // процесс/хост был реально приостановлен (сон ноутбука, пауза Docker
    // Desktop, вытеснение k8s-узла) дольше таймаута брокера — единственное
    // осмысленное лечение тут не "не дать порваться", а "само
    // восстановиться после разрыва". До этого фикса разорванное соединение
    // оставалось мёртвым навсегда, и весь пайплайн сообщений вставал до
    // ручного рестарта контейнера.
    QUrl m_url;
    QTimer m_reconnectTimer;
    int m_reconnectDelayMs = 2000;      // стартовая задержка
    static constexpr int kMaxReconnectDelayMs = 30000; // потолок бэкоффа
};

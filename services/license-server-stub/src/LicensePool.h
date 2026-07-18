#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUuid>

// In-memory пул фиктивных "лицензий" NX с TTL и ограниченным числом мест.
//
// Это Phase 0 заглушка (см. ARCHITECTURE.md: "in-memory (пока)") — реальная
// интеграция с Siemens FlexLM запланирована в ROADMAP.md на Phase 2.
// Сознательно не притворяется большим, чем оно есть: состояние живёт только
// в памяти процесса и теряется при рестарте (это ожидаемо для заглушки, а
// не забытый баг — при реальной интеграции источником истины станет сам
// FlexLM-сервер).
//
// При этом пул мест — не чистая бутафория: ограничение реально работает,
// и /checkout реально может отказать, когда все места заняты. Это позволяет
// уже сейчас проверить graceful-деградацию у клиентов (nx-worker-stub),
// не дожидаясь настоящего FlexLM.
class LicensePool : public QObject {
    Q_OBJECT
public:
    struct CheckoutResult {
        bool ok = false;
        QString token;
        QDateTime expiresAt;
    };

    LicensePool(int totalSeats, int ttlSeconds, QObject *parent = nullptr);

    CheckoutResult checkout(const QString &clientId, const QString &jobId);

    // true, если токен существовал (и ещё не истёк) и был освобождён.
    bool checkin(const QString &token);

    int totalSeats() const { return m_totalSeats; }
    int seatsInUse() const { return m_tokens.size(); }
    int ttlSeconds() const { return m_ttlSeconds; }

    // Публичный доступ к очистке протухших токенов — нужен для того, чтобы
    // GET /health и GET /seats отдавали актуальное число мест сразу после
    // истечения TTL, а не ждали ближайшего тика фонового таймера (до 10с).
    void refresh() { sweepExpired(); }

private slots:
    void sweepExpired();

private:
    struct Token {
        QString clientId;
        QString jobId;
        QDateTime expiresAt;
    };

    int m_totalSeats;
    int m_ttlSeconds;
    QHash<QString, Token> m_tokens; // token -> данные
    QTimer m_sweepTimer;
};

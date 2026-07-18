#include "LicensePool.h"

#include <QDebug>

LicensePool::LicensePool(int totalSeats, int ttlSeconds, QObject *parent)
    : QObject(parent), m_totalSeats(totalSeats), m_ttlSeconds(ttlSeconds) {
    // Проход по истёкшим токенам раз в 10 секунд. Без этого клиент, который
    // забыл сделать /checkin (или упал), навсегда занимал бы место — ровно
    // то поведение, которого TTL и призван не допустить.
    connect(&m_sweepTimer, &QTimer::timeout, this, &LicensePool::sweepExpired);
    m_sweepTimer.start(10000);
}

LicensePool::CheckoutResult LicensePool::checkout(const QString &clientId, const QString &jobId) {
    sweepExpired();

    CheckoutResult result;
    if (m_tokens.size() >= m_totalSeats) {
        result.ok = false;
        return result;
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDateTime expiresAt = QDateTime::currentDateTimeUtc().addSecs(m_ttlSeconds);
    m_tokens.insert(token, Token{clientId, jobId, expiresAt});

    result.ok = true;
    result.token = token;
    result.expiresAt = expiresAt;
    return result;
}

bool LicensePool::checkin(const QString &token) {
    sweepExpired();
    return m_tokens.remove(token) > 0;
}

void LicensePool::sweepExpired() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it.value().expiresAt <= now) {
            qInfo() << "[license-server-stub] token expired, releasing seat:" << it.key()
                    << "client=" << it.value().clientId << "job=" << it.value().jobId;
            it = m_tokens.erase(it);
        } else {
            ++it;
        }
    }
}

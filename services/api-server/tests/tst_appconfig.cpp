#include <QtTest>

#include "config/appConfig.h"
#include "config/env.h"

class TstAppConfig : public QObject {
    Q_OBJECT
private slots:
    // loadConfig() читает переменные окружения РОВНО ОДИН РАЗ и кладётся в
    // main.cpp в статическую переменную namespace-области видимости —
    // сам loadConfig() от этого не страдает и остаётся честной функцией
    // без скрытого состояния, так что тестировать его напрямую, вызывая
    // по многу раз с разным окружением, безопасно.
    void init() {
        // Гарантируем чистое окружение перед каждым тестом — иначе тест,
        // запущенный после другого (или в CI с ENV, унаследованным от
        // docker-compose), молча получил бы чужие значения вместо дефолтов.
        qunsetenv(Env::Port);
        qunsetenv(Env::DatabaseUrl);
        qunsetenv(Env::RabbitmqUrl);
        qunsetenv(Env::AuthServiceUrl);
    }

    void cleanup() {
        qunsetenv(Env::Port);
        qunsetenv(Env::DatabaseUrl);
        qunsetenv(Env::RabbitmqUrl);
        qunsetenv(Env::AuthServiceUrl);
    }

    void noEnv_fallsBackToDocumentedDefaults() {
        const AppConfig cfg = loadConfig();
        QCOMPARE(cfg.port, quint16(8080));
        QCOMPARE(cfg.rabbitmqUrl, QUrl("amqp://simensnx:simensnx@rabbitmq:5672/"));
        QCOMPARE(cfg.databaseUrl, QUrl("postgres://simensnx:simensnx@postgres:5432/simensnx"));
        QCOMPARE(cfg.authServiceUrl, QUrl("http://auth-stub:8081"));
    }

    void envVars_overrideDefaults() {
        qputenv(Env::Port, "9090");
        qputenv(Env::DatabaseUrl, "postgres://u:p@db-host:5432/mydb");
        qputenv(Env::RabbitmqUrl, "amqp://u:p@mq-host:5672/vhost");
        qputenv(Env::AuthServiceUrl, "http://auth.internal:9000");

        const AppConfig cfg = loadConfig();
        QCOMPARE(cfg.port, quint16(9090));
        QCOMPARE(cfg.databaseUrl, QUrl("postgres://u:p@db-host:5432/mydb"));
        QCOMPARE(cfg.rabbitmqUrl, QUrl("amqp://u:p@mq-host:5672/vhost"));
        QCOMPARE(cfg.authServiceUrl, QUrl("http://auth.internal:9000"));
    }

    // main.cpp явно проверяет "!config.rabbitmqUrl.isValid()" и падает при
    // старте, если PORT невалиден loadConfig() бросает исключение сам —
    // это первый и единственный рубеж защиты от "тихого" запуска на
    // случайном порту.
    void invalidPort_throws() {
        qputenv(Env::Port, "not-a-port");
        bool threw = false;
        try {
            loadConfig();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        QVERIFY2(threw, "loadConfig() должен бросать std::runtime_error при невалидном PORT");
    }

    void portOutOfRange_throws() {
        // toUShort() не примет отрицательные числа и значения > 65535 —
        // граничный случай, отдельный от "вообще не число".
        qputenv(Env::Port, "70000");
        bool threw = false;
        try {
            loadConfig();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        QVERIFY2(threw, "PORT вне диапазона 0..65535 должен считаться невалидным");
    }
};

QTEST_APPLESS_MAIN(TstAppConfig)
#include "tst_appconfig.moc"

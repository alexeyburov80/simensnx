#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

// Минимальный реестр метрик в формате Prometheus text exposition —
// сознательно без client_golang-подобной библиотеки: нужны только
// монотонные счётчики (Counter), без гистограмм и без многопоточности
// (весь наш стек — однопоточный QCoreApplication), так что достаточно
// QMap<строка, число> и ручной сериализации по HELP/TYPE.
//
// Header-only и дублируется в каждом сервисе так же, как HttpServer/
// QtAmqpConsumer — см. обоснование в docs/ADDING_A_SERVICE.md.
class Metrics {
public:
    // labels — например {{"job_type","process"}} — сериализуются в
    // jobs_published_total{job_type="process"}. Порядок меток
    // нормализуется через QMap (лексикографическая сортировка ключей),
    // чтобы одна и та же комбинация меток всегда давала один и тот же ключ
    // независимо от порядка передачи.
    void inc(const QString &name, const QString &help, const QMap<QString, QString> &labels = {}, double by = 1.0) {
        m_help[name] = help;
        m_type[name] = "counter";
        m_values[renderKey(name, labels)] += by;
        m_baseNameOf[renderKey(name, labels)] = name;
    }

    // В отличие от inc() — не накапливает, а перезаписывает текущее
    // значение. Для величин вроде "занято мест сейчас" или "глубина
    // очереди сейчас" counter был бы неверным типом метрики: он обязан
    // только расти, а эти значения естественным образом то растут, то
    // падают.
    void set(const QString &name, const QString &help, const QMap<QString, QString> &labels, double value) {
        m_help[name] = help;
        m_type[name] = "gauge";
        m_values[renderKey(name, labels)] = value;
        m_baseNameOf[renderKey(name, labels)] = name;
    }

    QByteArray render() const {
        // Группируем строки метрик по базовому имени, чтобы HELP/TYPE шли
        // один раз перед всеми label-комбинациями этой метрики — так того
        // требует сам формат exposition (иначе Prometheus просто это не
        // распарсит корректно).
        QMap<QString, QStringList> byBaseName; // baseName -> ["name{labels} value", ...]
        for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it) {
            const QString baseName = m_baseNameOf.value(it.key(), it.key());
            byBaseName[baseName] << QString("%1 %2").arg(it.key()).arg(it.value());
        }

        QByteArray out;
        for (auto it = byBaseName.constBegin(); it != byBaseName.constEnd(); ++it) {
            const QString &baseName = it.key();
            out += "# HELP " + baseName.toUtf8() + " " + m_help.value(baseName).toUtf8() + "\n";
            out += "# TYPE " + baseName.toUtf8() + " " + m_type.value(baseName, "counter").toUtf8() + "\n";
            for (const QString &line : it.value()) {
                out += line.toUtf8() + "\n";
            }
        }
        return out;
    }

private:
    static QString renderKey(const QString &name, const QMap<QString, QString> &labels) {
        if (labels.isEmpty()) return name;
        QStringList parts;
        for (auto it = labels.constBegin(); it != labels.constEnd(); ++it) {
            parts << QString("%1=\"%2\"").arg(it.key(), it.value());
        }
        return QString("%1{%2}").arg(name, parts.join(","));
    }

    QMap<QString, double> m_values;      // "name{labels}" -> значение
    QMap<QString, QString> m_baseNameOf; // "name{labels}" -> "name" (для группировки в render())
    QMap<QString, QString> m_help;       // "name" -> HELP-текст
    QMap<QString, QString> m_type;       // "name" -> "counter" | "gauge"
};

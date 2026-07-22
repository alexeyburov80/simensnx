#include <QtTest>

#include "../src/Metrics.h"

class TstMetrics : public QObject {
    Q_OBJECT
private slots:
    void inc_withoutLabels_accumulates() {
        Metrics m;
        m.inc("jobs_total", "help text");
        m.inc("jobs_total", "help text");
        m.inc("jobs_total", "help text", {}, 3.0);
        const QString out = QString::fromUtf8(m.render());
        QVERIFY(out.contains("# HELP jobs_total help text\n"));
        QVERIFY(out.contains("# TYPE jobs_total counter\n"));
        QVERIFY(out.contains("jobs_total 5\n"));
    }

    void set_overwritesRatherThanAccumulates() {
        // set() — для gauge: "занято сейчас", "глубина очереди сейчас".
        // В отличие от inc(), второй вызов должен ПЕРЕЗАПИСАТЬ значение,
        // а не прибавить к нему — иначе это был бы просто counter под
        // другим именем.
        Metrics m;
        m.set("queue_depth", "current depth", {}, 10);
        m.set("queue_depth", "current depth", {}, 3);
        const QString out = QString::fromUtf8(m.render());
        QVERIFY(out.contains("# TYPE queue_depth gauge\n"));
        QVERIFY(out.contains("queue_depth 3\n"));
        QVERIFY(!out.contains("queue_depth 10\n"));
    }

    void labels_areRenderedInPrometheusFormat() {
        Metrics m;
        m.inc("api_jobs_published_total", "help", {{"job_type", "process"}});
        const QString out = QString::fromUtf8(m.render());
        QVERIFY(out.contains(R"(api_jobs_published_total{job_type="process"} 1)"));
    }

    // renderKey() сортирует метки лексикографически по ключу через QMap —
    // значит одна и та же комбинация меток, переданная в разном порядке,
    // должна схлопнуться в одну и ту же серию, а не завести две разные
    // строки с одинаковым смыслом (частая причина задвоенных панелей в
    // Grafana).
    void labels_sameCombinationDifferentOrder_mergeIntoOneSeries() {
        Metrics m;
        m.inc("http_requests_total", "help", {{"method", "GET"}, {"path", "/jobs"}});
        m.inc("http_requests_total", "help", {{"path", "/jobs"}, {"method", "GET"}});
        const QString out = QString::fromUtf8(m.render());
        QVERIFY(out.contains(R"(http_requests_total{method="GET",path="/jobs"} 2)"));
        // Ровно одна серия, а не две с одинаковыми метками, но значением 1.
        QCOMPARE(out.count("http_requests_total{"), 1);
    }

    void differentLabelCombinations_areSeparateSeriesUnderOneHelpType() {
        Metrics m;
        m.inc("api_jobs_published_total", "help", {{"job_type", "process"}});
        m.inc("api_jobs_published_total", "help", {{"job_type", "validate"}});
        const QString out = QString::fromUtf8(m.render());
        // HELP/TYPE ровно один раз на базовое имя метрики (см. комментарий
        // в Metrics::render() — формат Prometheus этого требует).
        QCOMPARE(out.count("# HELP api_jobs_published_total"), 1);
        QCOMPARE(out.count("# TYPE api_jobs_published_total"), 1);
        QVERIFY(out.contains(R"(api_jobs_published_total{job_type="process"} 1)"));
        QVERIFY(out.contains(R"(api_jobs_published_total{job_type="validate"} 1)"));
    }

    void render_ofEmptyRegistry_isEmpty() {
        Metrics m;
        QVERIFY(m.render().isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstMetrics)
#include "tst_metrics.moc"

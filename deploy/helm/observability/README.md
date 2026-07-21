# Observability: Prometheus + Grafana + Loki в Kubernetes

Как и `postgres`/`rabbitmq` (см. их `TODO.md` по соседству), здесь
**нет собственных чартов** на сам Prometheus/Grafana/Loki — только values
поверх community-чартов. Причина та же: это готовые, годами поддерживаемые
компоненты с operator'ами (Prometheus Operator + CRD `ServiceMonitor`),
дашбордами и алертами по умолчанию — переписывать их с нуля означало бы
хуже и дольше решать уже решённую задачу.

В `docker-compose.yml` для локальной разработки то же самое собрано вручную
(без операторов — там простой pull по фиксированному списку таргетов), см.
`deploy/observability/prometheus.yaml` и `deploy/observability/promtail-config.yaml`.
Идея та же в обоих случаях: сервисы сами ничего никуда не отправляют, их
опрашивают/с них читают — pull, а не push (см.
`docs/adr/0003-logging-pull-not-push.md`).

## Порядок установки

```bash
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts
helm repo add grafana https://grafana.github.io/helm-charts
helm repo update

kubectl create namespace observability

helm install kube-prometheus-stack prometheus-community/kube-prometheus-stack \
  -n observability \
  -f deploy/helm/observability/kube-prometheus-stack-values.yaml

helm install loki grafana/loki \
  -n observability \
  -f deploy/helm/observability/loki-values.yaml

helm install promtail grafana/promtail \
  -n observability \
  -f deploy/helm/observability/promtail-values.yaml
```

`kube-prometheus-stack` тянет за собой и Grafana — отдельно её ставить не
нужно, `additionalDataSources` в values уже подключает Loki к ней.

## Дашборды

`deploy/observability/grafana-dashboards/jobs-pipeline.json` — уже готовый
дашборд по нашим метрикам (публикации, идемпотентные повторы, retry/dead_letter,
зависшие задачи, занятость лицензий). В docker-compose подключается сам
(см. `docker-compose.yml`, provisioning из этой же папки). Для k8s дашборд
нужно занести в кластер отдельно — `kube-prometheus-stack` включает
Grafana sidecar (`grafana.sidecar.dashboards.enabled: true` по умолчанию),
который сам находит `ConfigMap` с лейблом `grafana_dashboard: "1"`:

```bash
kubectl create configmap simensnx-jobs-pipeline \
  --from-file=deploy/observability/grafana-dashboards/jobs-pipeline.json \
  -n observability
kubectl label configmap simensnx-jobs-pipeline grafana_dashboard=1 -n observability
```

Дашборд подхватится в течение минуты без рестарта Grafana.

## Как сервисы становятся видны Prometheus

Каждый чарт в `deploy/helm/<service>` уже несёт аннотации
`prometheus.io/scrape`/`prometheus.io/port`/`prometheus.io/path` на поде —
этого достаточно для голого Prometheus (как в docker-compose). Но
`kube-prometheus-stack` из коробки полагается на CRD `ServiceMonitor`, а не
на аннотации — поэтому в `kube-prometheus-stack-values.yaml` дополнительно
перечислены `additionalServiceMonitors` на каждый сервис. Добавляете новый
сервис по `docs/ADDING_A_SERVICE.md` — не забудьте дописать для него
`ServiceMonitor` в этот список, аннотаций одних будет недостаточно именно
для этого стека.

## Открытый вопрос: метрики `nx-worker-stub`

`nx-worker-stub` (точнее, его прод-аналог — реальный NX-воркер) работает
**вне кластера**, на Windows VM/сервере (см.
`docs/adr/0002-windows-worker-outside-k8s.md`). Он отдаёт `/metrics` так же,
как и остальные сервисы, но Prometheus внутри кластера туда не достучится
без дополнительной настройки — либо Prometheus **federation** (кластерный
Prometheus скрейпит внешний Prometheus на Windows-хосте), либо
**Pushgateway** (воркер сам пушит метрики раз в N секунд — единственное
оправданное место во всей системе, где push, а не pull, был бы уместен,
именно потому что обычная pull-модель кластерного Prometheus сюда физически
не дотягивается). Не решено, оставлено на Phase 2 вместе с остальной
интеграцией реального NX-воркера.

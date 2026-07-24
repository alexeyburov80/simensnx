# ROADMAP

План поэтапного перехода от оболочки (заглушки) к рабочей системе.
Каждый пункт — отдельная задача/issue. Статус: `stub` (сделана заглушка) →
`in-progress` → `done`.

## Phase 0 — Оболочка (завершена)

Цель: контейнеры, контракты API/очередей/схемы БД существуют и разворачиваются
в Kubernetes, но без бизнес-логики. Все пункты закрыты — текущая стадия
теперь Phase 1 (см. ниже).

- [x] Структура монорепозитория
- [x] docker-compose для локальной разработки
- [x] Схема PostgreSQL: `jobs`, `job_attempts`
- [x] Helm-чарты для всех сервисов, кроме `nx-worker-stub` (сознательно —
      он вне k8s, см. `docs/adr/0002-windows-worker-outside-k8s.md`) —
      Deployment/Service, без HPA/PDB, как и планировалось; `job-state-service`
      и `license-server-stub` дополнительно защищены от `replicaCount > 1`
      прямо в шаблоне (`{{ fail ... }}`) — см. `docs/SCALING.md` за
      объяснением, почему это не просто перестраховка. `postgres`/`rabbitmq` —
      по-прежнему через community-операторы, не свои чарты (см. их `TODO.md`).
      Observability (Prometheus/Grafana/Loki) — `deploy/helm/observability/`,
      тоже через community-чарты, не свои.
- [x] `api-server`: эндпоинты `POST /jobs`, `GET /jobs/{id}` — реальные данные
      (не фиктивные, как планировалось изначально), по контракту в
      `services/api-server/openapi.yaml` (валидна по OpenAPI 3.0.3,
      проверено `openapi-spec-validator`); интерактивно — `localhost:8090`
      (Swagger UI, см. `docker-compose.yml`)
- [x] `auth-stub`: всегда пропускает запрос, но с реальным сетевым вызовом
      (чтобы protocol/latency были видны сразу) — `POST /verify`, контракт
      совпадает с тем, что дёргает `api-server` (`callAuthStub()`)
- [x] `job-state-service`: пишет и читает состояние в PostgreSQL; retry-цикл
      с backoff и dead_letter реализован раньше срока (был в Phase 1, см. ниже)
- [x] `rabbitmq`: single-node (compose) — очереди/обменники/DLX-policy в
      `deploy/rabbitmq/definitions.json` (см. `deploy/rabbitmq/README.md` за
      объяснением топологии и почему dead-lettering задан policy, а не
      аргументом очереди в коде). 3-node через Cluster Operator (k8s) —
      осталось, см. Phase 1 (это отдельный, более крупный пункт: сам
      кластер, а не топология очередей).
- [x] `nx-worker-stub`: читает очередь, спит N секунд, кладёт файл-заглушку
      через `file-storage-service` (реальный `PUT /files/{key}` с
      минимальным валидным STEP-заголовком, не вымышленная строка-ссылка,
      как было раньше), подтверждает (ack). При неудаче сохранения —
      ретраи с backoff (до 5 попыток), затем компенсирующий `checkin`
      лицензии и `reject` в DLQ вместо зависшего зарезервированного места.
- [x] `license-server-stub`: `/checkout` и `/checkin` возвращают фиктивный
      токен с TTL — контракт совпадает с тем, что вызывает `nx-worker-stub`
- [x] `file-storage-service`: upload/download/list поверх локального PV —
      `PUT`/`GET`/`DELETE /files/{key}` + `GET /files`, защита от path
      traversal, атомарная запись через `.tmp` + `rename`
- [x] Нужно добавить проект unit тестов QTest отдельно от основного кода —
      сделано для `api-server`: `services/api-server/tests/` (CMake-цели
      `tst_httptypes`, `tst_metrics`, `tst_appconfig`, `tst_jobrepository`,
      `tst_httpserver`, все — через `ctest`). Логика вынесена в статическую
      библиотеку `api-server-core`, не зависящую от amqpcpp, поэтому тесты
      собираются и гоняются отдельно от `api-server` (флаги
      `API_SERVER_BUILD_APP` / `API_SERVER_BUILD_TESTS` в
      `services/api-server/CMakeLists.txt`) — без сети до GitHub и без
      OpenSSL dev-заголовков. Остальные сервисы аналогичным проектом тестов
      пока не покрыты — отдельная задача.

## Технический долг (DRY/SOLID)

Сделано:
- [x] `common/` — общая библиотека для 6 сервисов: `HttpServer`/`HttpTypes`/
      `Metrics` (были побайтово идентичными копиями в каждом сервисе, версия
      `HttpServer` в `api-server` успела молча уйти вперёд — async-хендлеры,
      timeout guard), `RabbitTopics.h` (имена очередей/обменников, раньше
      разбросанные строковые литералы по `nx-worker-stub`/`job-state-service`),
      `GracefulShutdown` (self-pipe signal handling, был идентичен в 3
      сервисах) и `PostgresConnection` (`openDatabase()`, был побайтово
      идентичен в `api-server`/`job-state-service`). Docker build context у
      всех 6 сервисов теперь корень репозитория (не `services/<name>`) —
      иначе `common/` не видна `docker build`.
- [x] Удалена дублирующая `ExistingJob`/`findJobByIdempotencyKey` в
      `api-server/main.cpp` — использовался не тот же самый, а *копия* уже
      существовавшего `JobRepository::findByIdempotencyKey`. Теперь один
      источник истины.
- [x] `api-server/main.cpp`: 111-строчный обработчик `POST /jobs` и
      обработчик `GET /jobs/{id}` вынесены в `JobsController`
      (`src/jobs/JobsController.{h,cpp}`). Зависимости — через конструктор:
      `IJobPublisher&` (новый интерфейс в `common/JobPublisher.h` — DIP,
      разрывает зависимость от amqpcpp именно там, где она мешала тестам;
      `QtAmqpConnection` реализует его без единого изменения в логике,
      сигнатуры `publish()`/`isReady()` уже совпадали), `JobRepository&`,
      новый `AuthStubClient&` (был свободной функцией `callAuthStub()`).
      Заодно наконец реализован `JobRepository::findById()` (был объявлен,
      но не реализован — см. историю рефакторинга) — сигнатура
      переработана из `bool + QVariantMap&` в `JobLookup` (различает
      "не найдено" от "запрос упал", это было нужно для 404 vs 500 в
      обработчике). `main.cpp` сократился с 381 до ~120 строк. Добавлен
      `tst_jobscontroller.cpp` — 12 тестов на то, что раньше нельзя было
      протестировать в принципе (валидация, идемпотентность, fail-closed
      при недоступном auth-stub, маршрутизация по `job_type`, best-effort
      публикация в `jobs.events`), через SQLite + фейковый `IJobPublisher`
      + локальный HTTP-двойник auth-stub, без единого реального сетевого
      вызова наружу и без amqpcpp.

Осталось:
- [ ] `job-state-service/main.cpp` (459 строк) и `nx-worker-stub/main.cpp`
      (405 строк) — та же проблема, что была в `api-server` до вышеописанной
      декомпозиции: бизнес-логика живёт как вложенные лямбды внутри
      `main()`. План — тот же: классы-обработчики с зависимостями через
      конструктор, по образцу `JobsController`.
- [ ] `QtAmqpConsumer` в `job-state-service`/`nx-worker-stub` — тоже
      дублирование, но НЕ такое же чистое, как то, что уже вынесено: API
      успел разойтись (`job-state-service` умеет `consumeFromFanoutExchange`/
      `declareWorkQueues`, `nx-worker-stub` — нет). Объединение требует
      сначала спроектировать общий интерфейс, а не просто перенести файл.
- [ ] `config/http/httpStatus.h`/`config/http/routes.h` в `api-server` —
      мёртвый код: `HttpStatus::`/`Routes::`/`AuthRoutes::` не используются
      нигде, даже в самом `api-server` (проверено grep'ом) — везде голые
      магические числа (`200`, `404`, `503`...) и строки-литералы путей.
      Либо начать реально использовать, либо удалить — как есть сейчас,
      это незавершённый рефакторинг, который никому не помогает.

## Phase 1 — Инфраструктура становится настоящей

- [ ] PostgreSQL: перейти на HA (CloudNativePG или Patroni), настроить
      WAL-архивирование (pgBackRest/WAL-G), проверить PITR на тесте
- [ ] RabbitMQ: перейти на полноценный 3-нодовый кластер через RabbitMQ
      Cluster Operator, нагрузочный тест на потерю ноды
- [ ] Секреты: заменить голые k8s Secrets на Vault + External Secrets
      Operator (или облачный секрет-менеджер, если деплой в облако)
- [x] Observability: логи — Promtail/Loki/Grafana поверх stdout контейнеров
      (сделано раньше срока в Phase 0, см. `docs/adr/0003-logging-pull-not-push.md`;
      `log-collector-stub` из push-модели никогда не понадобился).
      Метрики (Prometheus, `/metrics` во всех сервисах + Grafana datasource) —
      тоже сделано раньше срока. Дашборды под конкретные счётчики ещё не
      собраны — метрики есть, готовых панелей в Grafana нет.
- [ ] Backup/DR: Velero для бэкапа состояния кластера и снапшотов PV

## Phase 2 — Бизнес-логика: обработка и валидация моделей

- [ ] Определить набор проверок NX-моделей (что именно валидируем —
      геометрия, стандарты именования, допуски и т.д.) — отдельная
      постановка задачи с профильным специалистом NX
- [ ] `nx-worker`: реальный запуск NX в headless/batch-режиме (NX Journal/
      `run_journal`), обработка ошибок процесса NX (краши, зависания)
- [ ] `license-server`: реальная интеграция с Siemens FlexLM, учёт
      количества seats. Graceful-деградация при нехватке лицензий (задача
      возвращается в очередь, а не падает) уже реализована и проверена на
      уровне заглушки — `nx-worker-stub` ретраит `/checkout` с бэкоффом
      вместо мгновенного провала задачи; при переходе на настоящий FlexLM
      этот механизм переиспользуется как есть.
- [x] `job-state-service`: retry с backoff (queued → processing → done|failed →
      retry или dead_letter, attempt_count/max_attempts) и sweep зависших в
      processing задач по таймауту — сделано. Идемпотентность по
      `idempotency_key` тоже сделана (проверка в api-server до публикации).
      Не сделано: работает только в 1 экземпляре (см. `docs/SCALING.md`),
      heartbeat от воркера нет — "завис" определяется по таймауту, а не по
      живому пульсу воркера.
- [ ] Версионирование NX Journal/скриптов: отдельный репозиторий или
      подпапка с тегами версий, воркер указывает, какую версию скрипта
      использовал при обработке (для воспроизводимости)

## Phase 3 — Продуктовая готовность

- [ ] `auth-stub` → полноценная аутентификация/авторизация (OAuth2/OIDC,
      например Keycloak), API-ключи для сервисных клиентов
- [ ] `file-storage-service` → при необходимости миграция на S3-совместимое
      хранилище (MinIO) без изменения контракта для клиентов
- [ ] Обратная связь клиенту: вебхуки или SSE в дополнение к polling
- [ ] Rate limiting и multi-tenancy в `api-server`
- [ ] CI/CD: пайплайны сборки образов, GitOps-деплой (ArgoCD/Flux) по тегам
      из монорепозитория
- [ ] Нагрузочное тестирование пула NX-воркеров, автоскейлинг пула
      (вне k8s — отдельный агент/скрипт масштабирования Windows-серверов)

## Открытые вопросы (требуют решения до начала Phase 2)

- Целевая платформа деплоя: облако (какое) или on-prem?
- Будет ли доступ в интернет на целевой площадке развёртывания? Разработка
  ведётся с доступом (сборка образов не меняется — `FetchContent`/`apt-get`
  внутри `docker build` это не проблема, раз сборочная машина в сети). Если
  на target его не будет: (1) готовые образы должны попасть туда не через
  `docker pull` из публичных registry — нужен внутренний registry или
  `docker save`/`docker load`; (2) Helm-чарты `postgres`/`rabbitmq` —
  community-операторы, сами тянущие свои образы из `quay.io` и т.п. прямо
  при `helm install` — это нужно будет либо смиррорить во внутренний
  registry, либо подтвердить, что на площадке интернет всё же будет. Не
  решается сейчас — целевая площадка ещё не выбрана.
- NX headless-режим подтверждён? Какие именно журналы/API используются?
- Условия лицензии Siemens: разрешена ли автоматизация в batch-режиме на
  выделенном сервере, как считаются seats при параллельной обработке?
- Формат и состав отчёта о результатах валидации модели (что видит клиент)?

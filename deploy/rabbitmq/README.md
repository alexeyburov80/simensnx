# RabbitMQ — топология (docker-compose)

`definitions.json` грузится брокером при старте через `RABBITMQ_LOAD_DEFINITIONS`
(см. `docker-compose.yml`, сервис `rabbitmq`). Пользователь и права на вход
сюда намеренно **не** входят — `RABBITMQ_DEFAULT_USER`/`RABBITMQ_DEFAULT_PASS`
из `docker-compose.yml` сами создают дефолтного пользователя с полным
доступом к vhost `/`; дублировать через `users`/`permissions` пришлось бы с
`password_hash` в специфичном для RabbitMQ формате, а не как plaintext-пароль
из `appConfig.cpp`.

## Топология

| Обменник (fanout, durable) | Кто публикует | Кто читает |
|---|---|---|
| `jobs.events` | `api-server` (`POST /jobs`) | `job-state-service` (очередь `jobs.job-state-service.intake`) |
| `jobs.status` | `nx-worker-stub` (после обработки) | `job-state-service` (очередь `jobs.job-state-service.status`) |
| `jobs.dlx` | RabbitMQ (dead-lettering) | очередь `jobs.dlq` |

| Очередь (durable) | Что в ней | Кто читает |
|---|---|---|
| `jobs.process` / `jobs.validate` | рабочие задания | `nx-worker-stub` |
| `jobs.dlq` | мёртвые письма | пока никто — инспекция вручную через management UI |
| `jobs.job-state-service.intake` | копия события создания задачи | `job-state-service` |
| `jobs.job-state-service.status` | копия статуса обработки | `job-state-service` |

Все перечисленные очереди/обменники/биндинги также объявляются идемпотентно
самими сервисами при подключении к брокеру (`declareQueue`/`declareExchange`/
`bindQueue` в коде) — `definitions.json` не единственный источник их
существования, а страховка на случай, если брокер поднимается позже
приложений или definitions.json меняется отдельно от кода. Расхождение между
ними не критично: RabbitMQ ничего не делает, если объект уже существует с
теми же аргументами.

## Единственное, что задаётся ТОЛЬКО здесь — policy `jobs-dead-lettering`

```json
"pattern": "^jobs\\.(process|validate)$",
"apply-to": "queues",
"definition": { "dead-letter-exchange": "jobs.dlx" }
```

Ни `api-server`, ни `nx-worker-stub` не указывают `x-dead-letter-exchange` в
аргументах при объявлении `jobs.process`/`jobs.validate` — специально: если
бы это было аргументом очереди, RabbitMQ требовал бы побитового совпадения
аргументов у всех, кто объявляет одну и ту же очередь (а её объявляют оба
сервиса независимо), и малейшее расхождение рвало бы канал с ошибкой
`PRECONDITION_FAILED`. Policy накладывается на уже существующую очередь без
пересоздания и без этого риска — см. также комментарий в
`services/api-server/src/QtAmqpConnection.cpp`.

Письмо попадает в `jobs.dlx` → `jobs.dlq`, когда `nx-worker-stub` делает
`reject(deliveryTag, /*requeue=*/false)` — это происходит на неисправимые
ошибки (битый JSON, отсутствующие обязательные поля); временные проблемы
(например, нет свободных лицензий) обрабатываются ретраями с backoff, а не
через DLQ.

## Как проверить руками

```bash
docker compose up -d rabbitmq
# management UI: http://localhost:15672 (simensnx/simensnx по умолчанию)
docker compose exec rabbitmq rabbitmqctl list_policies
docker compose exec rabbitmq rabbitmqctl list_queues name policy
```

`list_queues` должен показать `jobs-dead-lettering` в колонке `policy` для
`jobs.process` и `jobs.validate`, и пусто — для остальных очередей.

## Что здесь НЕ покрыто (см. ROADMAP.md)

3-нодовый кластер через RabbitMQ Cluster Operator в Kubernetes — отдельная
задача (Phase 1), не про docker-compose. Здесь только single-node топология
для локальной разработки.

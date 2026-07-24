# Сообщения в системе

Справочник по всем сообщениям, которыми обмениваются сервисы: HTTP-запросы/
ответы и AMQP-события, с точными полями и тем, кто что публикует/читает.
Источник истины — код (`src/main.cpp` каждого сервиса) и
`services/api-server/openapi.yaml`; этот файл — их читаемая сводка, при
расхождении смотрите код.

Топология очередей/обменников RabbitMQ (кто на что подписан, DLX-policy) —
отдельно в [`deploy/rabbitmq/README.md`](../deploy/rabbitmq/README.md). Здесь
— только содержимое сообщений и то, что с ними происходит по бизнес-логике.

## Быстрый ориентир: кто с кем говорит

```
                    POST /jobs                    GET /jobs/{id}
  Клиент  ────────────────────────▶  api-server  ◀──────────────  Клиент
                                         │  │
                            POST /verify │  │ читает jobs (RO)
                                         ▼  ▼
                                  auth-stub   PostgreSQL ◀── job-state-service
                                                                (владелец записи)
  api-server ──publish──▶ jobs.process/jobs.validate ──consume──▶ nx-worker-stub
  api-server ──publish──▶ jobs.events (fanout)        ──consume──▶ job-state-service
  nx-worker-stub ──publish──▶ jobs.status (fanout)    ──consume──▶ job-state-service

  nx-worker-stub ──POST /checkout, /checkin──▶ license-server-stub
  nx-worker-stub ──PUT /files/{key}──▶ file-storage-service
```

Каждый HTTP-сервис дополнительно отдаёт `GET /health` и `GET /metrics`
(Prometheus text exposition) — не расписаны отдельно ниже, формат
одинаковый везде: `{"status": "healthy"|"degraded", ...}` и текстовый дамп
метрик соответственно.

## Жизненный цикл задачи (`jobs.status` в PostgreSQL)

```
                    ┌─────────────────────────────────────────┐
                    │                                          │
                    ▼                                          │
  queued ──▶ processing ──▶ done                                │
                 │                                              │
                 ├──▶ failed ──▶ (attempt_count < max_attempts) ┘  (retry: queued)
                 │
                 └──▶ failed ──▶ (attempt_count >= max_attempts) ──▶ dead_letter
```

Решение "ретраить или сдаться" принимает **только** `job-state-service`,
функция `retryOrDeadLetter()` — по `attempt_count`/`max_attempts` из строки
в PostgreSQL. `nx-worker-stub` про эту логику ничего не знает: он просто
публикует `failed`, что происходит с задачей дальше — не его забота.

`processing` попадает в статус дважды с разным смыслом на входе:
`attempt_count` увеличивается именно в этот момент (не при `queued` и не
при `done`/`failed`) — см. `UPDATE jobs SET status = 'processing',
attempt_count = attempt_count + 1` в `job-state-service`.

Два способа попасть в `failed → retry/dead_letter`:
1. `nx-worker-stub` явно публикует `jobs.status` со `status: "failed"`
   (не хватило лицензии после всех попыток, не удалось сохранить результат).
2. `job-state-service` сам находит задачу, зависшую в `processing` дольше
   `JOB_STUCK_TIMEOUT_SECONDS` (по умолчанию 120с) — периодический sweep раз
   в 30с, воркер мог упасть, не прислав ничего. Обрабатывается **той же**
   `retryOrDeadLetter()`, что и явный `failed`.

## HTTP-сообщения

### `api-server` (публичная точка входа, порт 8080)

#### `POST /jobs`

Полная схема — в `openapi.yaml` (там же примеры). Кратко:

**Запрос:**
```json
{
  "client_id": "acme-corp",
  "job_type": "process",
  "input_file_ref": "models/part1.step",
  "idempotency_key": "order-42"
}
```
`job_type` — `process` (полный цикл: лицензия → "обработка" → результат) или
`validate` (Phase 0: всегда авто-одобряется, `nx-worker-stub` не делает для
этого типа вообще ничего, кроме ack). `idempotency_key` необязателен.

**Ответ 202** (принято, поставлено в очередь):
```json
{"job_id": "503ceb5c-...", "status": "queued", "queue": "jobs.process"}
```

**Ответ 200** (повтор с уже использованным `idempotency_key` — новая задача
НЕ создаётся, возвращается существующий `job_id` и его текущий статус,
каким бы он ни был):
```json
{"job_id": "503ceb5c-...", "status": "done", "idempotent_replay": true}
```

**Ответ 503** — `auth-stub` недоступен/отказал, либо канал к RabbitMQ ещё не
готов. В обоих случаях задача **не** публикуется — не бывает ситуации, когда
клиент получил 5xx, но задача на самом деле ушла в очередь.

**Ответ 400** — тело не JSON, нет обязательных полей, либо `job_type` не
`process`/`validate`.

Порядок действий внутри `api-server` важен для отладки: (1) реальный HTTP-
вызов `auth-stub` → (2) проверка `idempotency_key` по PostgreSQL (уже
существует — сразу ответ, без публикации) → (3) генерация `job_id` (UUID) →
(4) publish в `jobs.process`/`jobs.validate` → (5) publish копии в
`jobs.events`.

#### `GET /jobs/{id}`

Читает строку из PostgreSQL напрямую (не спрашивает `job-state-service` по
HTTP — оба сервиса просто читают одну и ту же таблицу).

```json
{
  "job_id": "503ceb5c-...",
  "client_id": "acme-corp",
  "job_type": "process",
  "status": "done",
  "input_file_ref": "models/part1.step",
  "result_file_ref": "results/503ceb5c-....step",
  "error_message": null,
  "attempt_count": 1,
  "max_attempts": 3,
  "created_at": "2026-07-22T10:00:00Z",
  "updated_at": "2026-07-22T10:00:05Z"
}
```
`result_file_ref`/`error_message` — `null`, пока задача не дошла до
`done`/`failed`/`dead_letter` соответственно. 404, если `id` не найден.

### `job-state-service` (владелец БД, порт 8082)

Тот же `GET /jobs/{id}` (идентичный ответ — та же таблица), плюс
внутренний `GET /jobs` без пути (последние 50 задач, `ORDER BY created_at
DESC`) — используется для отладки/дашбордов, в публичном `openapi.yaml`
намеренно не описан (это не контракт для внешних клиентов).

### `auth-stub` (порт 8081)

#### `POST /verify`

Всегда пропускает (Phase 0 — см. `ROADMAP.md`), но делает это через
реальный HTTP round-trip, а не заглушку в коде вызывающего сервиса — чтобы
задержка/логи сети были видны уже сейчас.

**Ответ 200** (всегда):
```json
{"allowed": true, "client_id": "a1b2c3d4e5f6", "note": "auth-stub Phase 0: no real validation, always allows"}
```
`client_id` тут — не то же самое, что `client_id` в `POST /jobs`: это
короткий SHA-256 хэш заголовка `Authorization` (если он был передан),
просто чтобы у ответа была структура, похожая на то, что вернёт настоящий
auth-сервис в Phase 3. `api-server` это поле сейчас никак не использует.

### `license-server-stub` (порт 8083)

#### `POST /checkout`

**Запрос:** `{"client_id": "acme-corp", "job_id": "503ceb5c-..."}`

**Ответ 200** (место выдано):
```json
{"token": "a1b2c3...", "expires_at": "2026-07-22T10:05:00Z", "ttl_seconds": 300}
```

**Ответ 503** (мест нет — **легитимный бизнес-ответ**, не ошибка сервиса;
вызывающий код обязан трактовать это как "попробуй позже", а не как отказ
задачи):
```json
{"error": "no license seats available", "seats_total": 3, "seats_in_use": 3}
```

#### `POST /checkin`

**Запрос:** `{"token": "a1b2c3..."}`

**Ответ 200:** `{"status": "released"}`
**Ответ 404** — токен не найден (уже освобождён по TTL раньше, чем пришёл
checkin) — это тоже честный ответ, не 500.

#### `GET /seats` (справочный, не используется другими сервисами программно)
```json
{"total": 3, "in_use": 1, "available": 2, "ttl_seconds": 300}
```

### `file-storage-service` (порт 8084)

#### `PUT /files/{key}`
Тело запроса — сырые байты файла (`Content-Type: application/octet-stream`,
без обёртки в JSON). `{key}` может содержать `/` (например,
`results/503ceb5c-....step`) — обрабатывается атомарно (запись во временный
файл + rename), защищено от path traversal (`sanitizeKey()`).

**Ответ 201:** `{"key": "results/503ceb5c-....step", "bytes": 187}`

#### `GET /files/{key}` — тело ответа — сырые байты файла. 404, если файла нет.

#### `DELETE /files/{key}` — 204 при успехе, 404 если файла нет.

#### `GET /files` — список всех файлов:
```json
[{"key": "results/503ceb5c-....step", "bytes": 187}, ...]
```

## AMQP-сообщения

Все — JSON в теле сообщения, `Content-Type` AMQP-CPP не проставляет
отдельно (парсится как есть на стороне потребителя). Топология
(exchange/queue, DLX) — в `deploy/rabbitmq/README.md`.

### `jobs.process` / `jobs.validate` (work-очереди)

Публикует `api-server` при создании задачи **и** `job-state-service` при
ретрае (`retryOrDeadLetter()` republish'ит **то же** сообщение при новой
попытке). Единственный потребитель — `nx-worker-stub`.

```json
{
  "job_id": "503ceb5c-...",
  "client_id": "acme-corp",
  "job_type": "process",
  "input_file_ref": "models/part1.step",
  "idempotency_key": "order-42"
}
```
`idempotency_key` присутствует, только если был передан в `POST /jobs`.
При ретрае `job-state-service` формирует то же сообщение заново из строки
в PostgreSQL — без `idempotency_key` (он там для этого и не нужен, задача
уже существует).

### `jobs.events` (fanout — копия события создания задачи)

Публикуется `api-server` **одновременно** с публикацией в рабочую очередь,
тем же телом сообщения (см. выше). Единственный потребитель —
`job-state-service` (очередь `jobs.job-state-service.intake`), который на
основе этого события делает `INSERT INTO jobs (...) ON CONFLICT DO
NOTHING` — создаёт запись задачи со `status = 'queued'`.

Если эта публикация не удалась (в отличие от публикации в рабочую очередь),
`api-server` не проваливает запрос — задача всё равно уйдёт в обработку,
просто запись в БД появится с опозданием. Сознательный компромисс Phase 0.

### `jobs.status` (fanout — смена статуса от воркера)

Публикует **только** `nx-worker-stub`. Единственный потребитель —
`job-state-service` (очередь `jobs.job-state-service.status`).

```json
{"job_id": "503ceb5c-...", "status": "processing"}
```
```json
{"job_id": "503ceb5c-...", "status": "done", "result_file_ref": "results/503ceb5c-....step"}
```
```json
{"job_id": "503ceb5c-...", "status": "failed", "error_message": "no license seat available after retries"}
```

`result_file_ref` присутствует только при `status: "done"`, `error_message`
— только при `"failed"`. Оба поля опускаются целиком, если пустые (не
передаются как `null` — их просто нет в объекте).

### `jobs.dlx` / `jobs.dlq` (мёртвые письма)

Не JSON-сообщение отдельного формата — та же полезная нагрузка, что была в
`jobs.process`/`jobs.validate`/`jobs.events`/`jobs.status`, просто
маршрутизированная туда RabbitMQ-policy вместо потребителя. Происходит,
когда любой consumer делает `reject(deliveryTag, /*requeue=*/false)` —
только для **не подлежащих исправлению повтором** ошибок (битый JSON,
отсутствуют обязательные поля, исчерпаны все ретраи бизнес-логики). Сейчас
никто `jobs.dlq` не читает — инспекция вручную через management UI
(`rabbitmqctl list_queues`, см. `deploy/rabbitmq/README.md`).

## Гарантии доставки и идемпотентность

- **At-least-once, не exactly-once.** Каждый consumer обязан быть готов
  получить одно и то же сообщение дважды (`redelivered` в параметрах
  колбэка — так и называется в коде). Источники дублей: сеть моргнула между
  `basic.ack` и получением подтверждения брокером; воркер упал после
  обработки, но до `ack`; RabbitMQ requeue по внутреннему таймауту.
- **`idempotency_key`** — единственная защита от того, что **клиент**
  дважды создаст одну и ту же задачу (например, повторный HTTP-запрос при
  таймауте на его стороне). Проверяется в `api-server` **до** публикации —
  если ключ уже использован, повторной публикации не будет вообще, вернётся
  тот же `job_id`.
- **Гонка `jobs.events`/`jobs.status`.** Оба обменника — fanout, гарантий
  порядка между ними нет: теоретически `jobs.status` (`processing`) может
  прийти в `job-state-service` раньше, чем `jobs.events` успеет создать
  строку в БД. `unknownJobRetries` в `job-state-service` — счётчик попыток
  на этот случай: requeue с экспоненциальным backoff (до 8 попыток, максимум
  15с между ними), потом `reject` без requeue — защита от бесконечного
  busy-loop, если рассинхронизация не временная гонка, а постоянный баг.

## Коды ответов — что они означают в этой системе

| Код | Где | Значение |
|---|---|---|
| 503 | `POST /jobs` | `auth-stub` недоступен/отказал ИЛИ канал RabbitMQ не готов — задача не создана |
| 503 | `POST /checkout` | Нет свободных мест лицензии **сейчас** — легитимный ответ, не ошибка; клиент обязан ретраить с backoff |
| 404 | `POST /checkin` | Токен уже истёк по TTL и был освобождён раньше — не ошибка сервера |
| 404 | `GET /jobs/{id}` | Задачи с таким id нет |
| 400 | везде | Тело запроса невалидно (не JSON / нет обязательных полей) |
| 200 vs 202 | `POST /jobs` | 202 — новая задача создана; 200 — `idempotent_replay`, ничего не создавалось |

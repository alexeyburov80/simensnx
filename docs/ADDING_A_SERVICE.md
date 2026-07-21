# Добавление нового сервиса или новой очереди

## Новый HTTP-сервис

Все текущие сервисы построены по одному шаблону — копируете ближайший по
смыслу аналог, а не пишете с нуля.

1. **Выберите основу**:
   - Только HTTP, без сети наружу из обработчиков → `services/auth-stub`
     (синхронный `HttpServer`, самый простой пример).
   - HTTP + свои сетевые/БД-вызовы из обработчика (auth-check, чтение из
     Postgres и т.п.) → `services/api-server` (асинхронный `HttpServer` с
     `RespondFn`-колбэком).
   - HTTP + запись/чтение своей таблицы в Postgres → `services/job-state-service`.
   - Публикация в RabbitMQ → переиспользуйте `QtAmqpConnection`
     (`services/api-server/src/QtAmqpConnection.*`).
   - Потребление из RabbitMQ → переиспользуйте `QtAmqpConsumer`
     (`services/nx-worker-stub/src/QtAmqpConsumer.*`, или версию из
     `services/job-state-service`, если нужна ещё и подписка на fanout-обменники).

   Копирование `HttpServer.h/.cpp`, `QtAmqpConnection.*`/`QtAmqpConsumer.*`
   между сервисами — осознанное решение, а не недосмотр: сервисы должны
   разворачиваться и обновляться независимо. Общая библиотека имеет смысл,
   когда таких копий станет много и они начнут расходиться в багфиксах —
   этот момент ещё не наступил.

2. **CMakeLists.txt** — компоненты Qt5 по потребности (`Core` всегда,
   `Network` для HTTP/HTTP-клиента, `Sql` для Postgres). Если нужен AMQP —
   тот же блок `FetchContent` с пином `GIT_TAG v4.3.27`, что и в остальных
   сервисах (AMQP-CPP не пакетирован в apt ни для Debian bookworm, ни для
   Ubuntu 24.04 — проверено, появляется только в Debian trixie/Ubuntu 26.04).

3. **Dockerfile** — двухстадийный, `debian:bookworm` для сборки →
   `debian:bookworm-slim` для рантайма. Рантайм-пакеты по потребности:
   `libqt5core5a libqt5network5` всегда, `+libqt5sql5-psql` для Postgres,
   `+libssl3` для AMQP-CPP (звено TLS-кода компилируется, даже если сам
   сервис использует только `amqp://`, а не `amqps://`). Обязательно
   непривилегированный пользователь (`useradd --system ... && USER nx`).

4. **`docker-compose.yml`**:
   - `restart: unless-stopped` — см. `docs/SCALING.md`.
   - `depends_on` — только на реальные зависимости, не копируйте блок
     бездумно (пример реальной ошибки, которую нашли и убрали: `auth-stub`
     одно время зависел от `postgres`, хотя вообще с ним не работает).
   - Порт — сейчас заняты 8080 (api-server), 8081 (auth-stub), 8082
     (job-state-service), 8083 (license-server-stub), 8084 (file-storage-service).

5. **Логирование** — ничего не настраивать. Promtail видит новый контейнер
   через Docker service discovery автоматически, как только сервис появился
   в `docker-compose.yml` (см. `docs/adr/0003-logging-pull-not-push.md`).

6. **Владение данными в Postgres** — до того, как писать первый `INSERT`,
   решите: сервис единственный писатель в свою таблицу, или только читает
   чужую? Двух писателей в одну таблицу в этой архитектуре сознательно не
   заводили (пример: `api-server` читает `jobs`, но пишет туда только
   `job-state-service`).

7. **Helm** — на данный момент полноценный чарт есть только у `api-server`
   (`deploy/helm/api-server`), возьмите за образец; остальные — TODO-заготовки.

## Новая очередь RabbitMQ

Два разных паттерна, оба уже есть в коде как рабочий пример.

### А. Рабочая очередь с конкурирующими consumer'ами
(пример: `jobs.process`/`jobs.validate`)

```cpp
m_channel->declareQueue("my.queue", AMQP::durable);
m_channel->setQos(prefetch); // 1 для тяжёлой работы, больше — для лёгкой
```

**Критично**: если очередь объявляют несколько разных сервисов (как сейчас
`jobs.process` объявляют и `api-server`, и `nx-worker-stub`), аргументы
объявления должны быть **побитово одинаковыми** у всех — иначе RabbitMQ
рвёт канал с `PRECONDITION_FAILED` (406). Именно поэтому DLX подключается
не аргументом очереди в коде, а RabbitMQ policy (см. ниже) — policy можно
менять и накладывать на уже существующие очереди, не трогая объявление.

### Б. Fanout-копия для нескольких независимых подписчиков
(пример: `jobs.events`, `jobs.completed`)

Публикующая сторона:
```cpp
m_channel->declareExchange("my.event", AMQP::fanout, AMQP::durable);
m_channel->publish("my.event", "", body); // routing key игнорируется fanout'ом
```

Каждый подписчик заводит **свою** durable-очередь и биндит её к обменнику
(готовый метод — `QtAmqpConsumer::consumeFromFanoutExchange` в
`services/job-state-service/src/QtAmqpConsumer.*`):
```cpp
amqp.consumeFromFanoutExchange("my.event", "my-service.my-event.intake", prefetch, handler);
```

### Персистентность и DLQ

- Публикуете что-то, что должно пережить рестарт брокера — оборачивайте в
  `AMQP::Envelope` и зовите `envelope.setPersistent(true)` перед `publish()`
  (см. `QtAmqpConnection::publish`/`QtAmqpConsumer::publish`). `durable`
  описывает саму очередь, а не её содержимое — это разные вещи.
- Хотите, чтобы отклонённые (`reject(tag, false)`) или просроченные по TTL
  сообщения новой очереди не пропадали немо, а оседали в `jobs.dlq` —
  добавьте имя очереди в regex `pattern` политики
  `deploy/rabbitmq/definitions.json` (`^jobs\.(process|validate|job-state-service\..*)$`)
  и перезапустите RabbitMQ (или примените policy через
  `rabbitmqctl set_policy`/management API без рестарта брокера).

### Heartbeat и автопереподключение

Не пишите заново — `QtAmqpConnection`/`QtAmqpConsumer` уже умеют и то, и
другое (согласование интервала с брокером, реальная отправка
heartbeat-фреймов, переподключение с экспоненциальным бэкоффом при разрыве).
Если пишете AMQP-интеграцию для нового сервиса с нуля, а не копируете
существующий класс — скопируйте, это не тривиальная логика, и на ней уже
нашли и исправили не один баг.

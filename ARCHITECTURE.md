# Архитектура

## Обзор

```
Клиент
  │ HTTPS
  ▼
API server ──► Auth stub (проверка токена)
  │
  ▼ publish
RabbitMQ кластер (quorum queues: jobs.process, jobs.validate, jobs.dlq)
  │
  ├─ consume ──► NX worker (Windows, вне k8s-кластера)
  │                 │
  │                 ├─► License server stub (checkout/checkin лицензии)
  │                 └─► File storage service (чтение/запись файлов моделей)
  │
  └─ статус задачи пишется в PostgreSQL (через Job orchestrator)

Job orchestrator — следит за жизненным циклом задачи (jobs, job_attempts),
делает retry, возвращает "зависшие" задачи в очередь.

Log collector stub — принимает логи от всех сервисов (пока — заглушка).
```

## Компоненты

| Компонент | Ответственность | Хранит состояние | Куда деплоится |
|---|---|---|---|
| `api-server` | Приём запросов клиентов, публикация задач в очередь, отдача статуса | нет (читает из PostgreSQL) | k8s Deployment |
| `auth-stub` | Проверка токена/API-key (заглушка) | нет | k8s Deployment |
| `job-orchestrator` | State machine задачи, retry, идемпотентность, обработка таймаутов | PostgreSQL (`jobs`, `job_attempts`) | k8s Deployment |
| `rabbitmq` | Очереди задач, гарантия доставки | сам кластер (quorum queues) | k8s, RabbitMQ Cluster Operator |
| `postgres` | Состояние задач, метаданные | PostgreSQL сам | k8s, CloudNativePG/Patroni |
| `file-storage-service` | HTTP-доступ к файлам моделей поверх PV | файловая система на PV | k8s Deployment + PVC |
| `license-server-stub` | Резервирование "лицензий" NX (заглушка) | in-memory (пока) | k8s Deployment |
| `nx-worker-stub` | Разбор очереди, обработка модели, запись результата | нет (статус — через API) | Windows VM/сервер, **вне k8s** |

## Ключевые решения (см. также `docs/adr/`)

- **Kubernetes** — оркестратор для всех Linux-компонентов. Windows NX-воркеры
  сознательно вынесены за пределы кластера — см. `docs/adr/0002-windows-worker-outside-k8s.md`.
- **Логирование**: pull-модель (Promtail забирает stdout контейнеров, Loki
  хранит, Grafana показывает), а не push-сервис — см.
  `docs/adr/0003-logging-pull-not-push.md`.
- **RabbitMQ**: 3-нодовый кластер, quorum queues (не classic mirrored), DLX для
  сообщений, превысивших лимит доставок.
- **PostgreSQL**: в проде — HA через Patroni или CloudNativePG-оператор,
  WAL-архивирование через pgBackRest/WAL-G для PITR.
- **Файловое хранилище**: на старте — локальный PV + собственный HTTP-сервис
  (`file-storage-service`), контракт спроектирован так, чтобы позже подменить
  реализацию на S3-совместимое хранилище (MinIO) без изменения клиентов.
- **Секреты**: пока — обычные Kubernetes Secrets, с пометкой на замену
  Vault/External Secrets Operator (см. ROADMAP).

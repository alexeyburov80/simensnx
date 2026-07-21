# SimensNX — Design Automation Platform

Монорепозиторий системы автоматизации обработки и проверки моделей Siemens NX.

## Структура репозитория

```
simensnx/
├── README.md               — этот файл
├── ARCHITECTURE.md         — описание компонентов и их взаимодействия
├── ROADMAP.md              — план перехода от заглушек к реальной реализации
├── docs/
│   ├── adr/                 — architecture decision records
│   ├── SCALING.md           — какие сервисы можно масштабировать, какие нет
│   └── ADDING_A_SERVICE.md  — как добавить новый сервис/очередь по шаблону
├── docker-compose.yml      — локальное окружение для разработки (не прод!)
├── .env.example            — пример переменных окружения
├── services/                — исходники всех сервисов (кроме NX-воркера — Windows-контур)
│   ├── api-server/          — HTTP API для клиентов
│   ├── auth-stub/           — заглушка авторизации/аутентификации
│   ├── job-state-service/    — управление жизненным циклом задач, ретраи
│   ├── license-server-stub/ — заглушка Siemens license server
│   ├── file-storage-service/— HTTP-обёртка над файловым хранилищем
│   ├── log-collector-stub/  — заглушка сбора логов
│   └── nx-worker-stub/      — воркер NX (целевая ОС — Windows, здесь линуксовая заглушка для локальной проверки протокола)
├── db/
│   └── migrations/          — SQL-миграции PostgreSQL (plain SQL, накатываются вручную/через миграционный инструмент — выбрать в рамках ROADMAP)
├── deploy/
│   └── helm/                 — Helm-чарты для развёртывания каждого сервиса в Kubernetes
└── docs/
    └── adr/                  — Architecture Decision Records — зафиксированные архитектурные решения
```

## Быстрый старт (локально, для разработки)

```bash
cp .env.example .env
docker compose up --build
```

Поднимет: PostgreSQL (single-node, НЕ HA — это только для локальной разработки),
RabbitMQ (single-node), и все Linux-сервисы-заглушки.

Для прод-развёртывания используются чарты из `deploy/helm/` в Kubernetes-кластере
(HA PostgreSQL, RabbitMQ-кластер — см. `ARCHITECTURE.md`).

## Статус проекта

Сейчас — стадия "оболочка": все сервисы, кроме PostgreSQL, являются заглушками
(логируют вызовы, не содержат бизнес-логики). См. `ROADMAP.md` для плана
поэтапной замены заглушек на реальную реализацию.

# nx-worker-stub

**Важно**: целевая платформа для реального воркера — **Windows** (сервер NX
доступен только под Windows). Docker-контейнер в этой папке — линуксовая
заглушка **только для локальной проверки протокола** (consume из RabbitMQ,
запись в `file-storage-service`, вызов `license-server-stub`), она не
участвует в проде.

Реальный Windows-воркер (Phase 2 из ROADMAP.md) не будет контейнером в
Kubernetes — см. `docs/adr/0002-windows-worker-outside-k8s.md`. Планируется
как служба/процесс на Windows-сервере, использующая тот же протокол
(RabbitMQ consumer + HTTP-клиент к `file-storage-service`/
`license-server`).

Собрать и запустить эту Linux-заглушку локально можно так же, как остальные
сервисы (`docker compose up nx-worker-stub`), но `docker-compose.yml` не
включает её в цепочку прод-деплоя.

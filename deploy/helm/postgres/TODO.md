# postgres — TODO

Не пишем свой чарт. Используем CloudNativePG (или Patroni) оператор +
values для HA (1 primary + 2 реплики), WAL-архивирование через
pgBackRest/WAL-G. См. ARCHITECTURE.md и ROADMAP.md Phase 1.

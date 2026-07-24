#pragma once

// Единый источник имён очередей/обменников RabbitMQ. До этого файла
// api-server держал свою копию в config/rabbit/rabbitConfig.h (как
// RabbitConfig, единственный потребитель — сам api-server), а
// nx-worker-stub/job-state-service просто писали строки "jobs.process",
// "jobs.status" и т.п. россыпью по коду в нескольких местах каждый —
// опечатка в такой строке не поймалась бы компилятором, просто consumer
// тихо перестал бы получать события. Полная топология (кто на что
// подписан, DLX-policy) — в deploy/rabbitmq/README.md и docs/MESSAGES.md;
// здесь только имена.
namespace RabbitTopics {

// Work-очереди — публикует api-server (создание) и job-state-service
// (republish при retry), читает nx-worker-stub.
inline constexpr auto ProcessQueue = "jobs.process";
inline constexpr auto ValidateQueue = "jobs.validate";

// Fanout-обменники.
inline constexpr auto EventsExchange = "jobs.events"; // api-server -> job-state-service (intake)
inline constexpr auto StatusExchange = "jobs.status";  // nx-worker-stub -> job-state-service

// Именованные durable-очереди, которыми job-state-service биндится на
// fanout-обменники выше (competing consumers при нескольких репликах —
// см. обсуждение масштабирования в docs/SCALING.md).
inline constexpr auto JobStateIntakeQueue = "jobs.job-state-service.intake";
inline constexpr auto JobStateStatusQueue = "jobs.job-state-service.status";

// Dead-lettering: маршрутизация в jobs.dlx настроена RabbitMQ policy
// (deploy/rabbitmq/definitions.json), а не аргументом очереди в коде — имя
// exchange'а и очереди здесь исключительно для полноты, ни один сервис
// не публикует и не консьюмит их напрямую.
inline constexpr auto DeadLetterExchange = "jobs.dlx";
inline constexpr auto DeadLetterQueue = "jobs.dlq";

} // namespace RabbitTopics

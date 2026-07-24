#pragma once

#include <QString>

#include "RabbitTopics.h"

// Поля остаются QString (не inline constexpr), потому что RabbitConfig —
// часть AppConfig и исторически задумывался как runtime-конфигурируемый
// (хотя сейчас appConfig.cpp ничего из этого не переопределяет через env —
// см. её реализацию). Источник имён по умолчанию теперь один —
// common/RabbitTopics.h, а не отдельная копия строк здесь.
struct RabbitConfig
{
    QString processQueue = RabbitTopics::ProcessQueue;

    QString validateQueue = RabbitTopics::ValidateQueue;

    QString eventsExchange = RabbitTopics::EventsExchange;

    QString deadLetterExchange = RabbitTopics::DeadLetterExchange;

    QString deadLetterQueue = RabbitTopics::DeadLetterQueue;
};

#ifndef ENV_H
#define ENV_H

#pragma once

namespace Env
{
inline constexpr auto Port = "PORT";

inline constexpr auto DatabaseUrl = "DATABASE_URL";

inline constexpr auto RabbitmqUrl = "RABBITMQ_URL";

inline constexpr auto AuthServiceUrl = "AUTH_SERVICE_URL";
}

#endif // ENV_H

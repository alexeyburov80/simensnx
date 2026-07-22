#pragma once

namespace Routes
{
inline constexpr auto Root = "/";

inline constexpr auto Health = "/health";

inline constexpr auto Metrics = "/metrics";

inline constexpr auto Jobs = "/jobs";
}

namespace AuthRoutes
{
inline constexpr auto Verify = "/verify";
}

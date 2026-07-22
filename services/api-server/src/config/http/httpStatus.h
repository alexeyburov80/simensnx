#pragma once

namespace HttpStatus
{
    constexpr int Ok = 200;
    constexpr int Accepted = 202;

    constexpr int BadRequest = 400;
    constexpr int NotFound = 404;
    constexpr int MethodNotAllowed = 405;

    constexpr int PayloadTooLarge = 413;
    constexpr int HeaderTooLarge = 431;

    constexpr int InternalServerError = 500;

    constexpr int ServiceUnavailable = 503;

    constexpr int GatewayTimeout = 504;
}

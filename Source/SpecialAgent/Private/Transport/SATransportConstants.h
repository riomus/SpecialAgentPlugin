#pragma once

#include "CoreMinimal.h"

namespace SATransport
{
    // SSE keep-alive frame cadence. Must be shorter than the client's
    // HTTP-read-idle timeout. Claude Code's undici-based client drops streams
    // that go silent for more than a few seconds, so 5 s is conservative.
    // Bumping this also bumps the worst-case editor-shutdown wait.
    constexpr int32 KeepAliveIntervalSeconds = 5;

    // Maximum concurrent TCP connections. Editor use never approaches this;
    // the 17th client gets 503 Service Unavailable.
    constexpr int32 MaxConnections = 16;

    // HTTP request limits (v1 rejects anything larger).
    constexpr int32 MaxHeaderBytes = 16 * 1024;         // 16 KiB
    constexpr int32 MaxBodyBytes   = 16 * 1024 * 1024;  // 16 MiB

    // Socket read idle timeout before we close the connection.
    constexpr int32 IdleReadTimeoutSeconds = 30;

    // Per-read socket wait. Short so we can respond to bClientGone / shutdown
    // quickly without tight-looping.
    constexpr int32 SocketPollMilliseconds = 100;
}

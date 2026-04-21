#pragma once
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Transport/SAHttpResponse.h"

class FSocket;
class FSATcpServer;
class FMCPRequestRouter;

/**
 * One FRunnable per accepted TCP connection. Parses HTTP, dispatches to the
 * router, writes response. In v1 this handles single-body responses only;
 * Phase 2 adds SSE response mode, Phase 3 adds long-lived GET /sse.
 */
class FSAConnection : public FRunnable
{
public:
    FSAConnection(FSATcpServer* InOwner,
                  FSocket* InSocket,
                  TSharedPtr<FMCPRequestRouter> InRouter);
    virtual ~FSAConnection();

    // FRunnable
    virtual uint32 Run() override;
    virtual void Stop() override { bStopping = true; }

    /** Request that the connection close ASAP from outside. */
    void RequestStop() { bStopping = true; }

    /** Called by FSASessionRegistry when a newer GET /sse replaces this one. */
    void OnReplacedBySessionRegistry();

    /** Thread-safe: write an SSE event frame (used by notifications). */
    bool PushSSEEvent(const FString& EventName, const FString& Data);

private:
    // Phase-1 internals; grown in later phases.
    void HandleGetHealth();
    void HandleOptions();
    void HandlePostMCP(const struct FSAHttpRequest& Req);
    void HandlePostMCPSSE(const struct FSAHttpRequest& Req);
    void HandleNotFound();

    bool ReadFullRequest(TArray<uint8>& OutBuffer, struct FSAHttpRequest& OutReq);

    FSATcpServer* Owner;
    FSocket* Socket;
    TSharedPtr<FMCPRequestRouter> Router;
    FSAHttpResponse Writer;
    FThreadSafeBool bStopping { false };
    FString ActiveSessionIdForStream;   // set only when acting as GET /sse
};

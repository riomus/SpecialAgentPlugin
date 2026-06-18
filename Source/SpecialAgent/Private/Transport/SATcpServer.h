#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include <atomic>

class FTcpListener;
class FSocket;
struct FIPv4Endpoint;
class FSAConnection;
class FMCPRequestRouter;
class FRunnableThread;

class FSATcpServer
{
public:
    FSATcpServer(TSharedPtr<FMCPRequestRouter> InRouter);
    ~FSATcpServer();

    /** Bind to BindAddress ("127.0.0.1" default / "0.0.0.0" or "any" for all
     *  interfaces). When AuthToken is non-empty, requests must carry a matching
     *  "Authorization: Bearer <token>" header. */
    bool Start(int32 Port, const FString& BindAddress, const FString& AuthToken);
    void Stop();
    int32 GetActiveConnectionCount() const;

    /** Shared-secret token connections must present (empty = disabled). */
    const FString& GetAuthToken() const { return AuthToken; }

    /** Called by an FSAConnection when it exits, to remove itself from the registry. */
    void Retire(FSAConnection* Conn);

private:
    bool HandleAccept(FSocket* ClientSocket, const FIPv4Endpoint& Endpoint);

    TSharedPtr<FMCPRequestRouter> Router;
    TUniquePtr<FTcpListener> Listener;

    struct FActive
    {
        FSAConnection* Conn;
        FRunnableThread* Thread;
    };
    mutable FCriticalSection ActiveLock;
    TArray<FActive> Active;
    std::atomic<bool> bRunning { false };
    FString AuthToken;
};

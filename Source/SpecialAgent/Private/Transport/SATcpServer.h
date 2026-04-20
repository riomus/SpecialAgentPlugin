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

    bool Start(int32 Port);
    void Stop();
    int32 GetActiveConnectionCount() const;

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
};

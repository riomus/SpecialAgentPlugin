#include "Transport/SATcpServer.h"
#include "Transport/SAConnection.h"
#include "Transport/SATransportConstants.h"
#include "Common/TcpListener.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

FSATcpServer::FSATcpServer(TSharedPtr<FMCPRequestRouter> InRouter) : Router(InRouter) {}
FSATcpServer::~FSATcpServer() { Stop(); }

bool FSATcpServer::Start(int32 Port)
{
    if (bRunning.load()) return false;
    FIPv4Endpoint Endpoint(FIPv4Address::Any, Port);
    Listener = MakeUnique<FTcpListener>(Endpoint);
    Listener->OnConnectionAccepted().BindRaw(this, &FSATcpServer::HandleAccept);
    if (!Listener->Init()) { Listener.Reset(); return false; }
    bRunning.store(true);
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: raw TCP transport listening on %d"), Port);
    return true;
}

bool FSATcpServer::HandleAccept(FSocket* ClientSocket, const FIPv4Endpoint& Endpoint)
{
    FScopeLock L(&ActiveLock);
    if (Active.Num() >= SATransport::MaxConnections)
    {
        // Write a plain 503 directly to the raw socket without spawning a thread,
        // then destroy it. Matches the spec's error table for too-many-connections.
        const char* Msg =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 23\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
            "service temporarily busy";
        int32 Sent = 0;
        ClientSocket->Send((const uint8*)Msg, FCStringAnsi::Strlen(Msg), Sent);
        if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
            SS->DestroySocket(ClientSocket);
        return true;
    }
    FSAConnection* Conn = new FSAConnection(this, ClientSocket, Router);
    FRunnableThread* T = FRunnableThread::Create(Conn,
        *FString::Printf(TEXT("SAConn-%s"), *Endpoint.ToString()));
    Active.Add({Conn, T});
    return true;
}

void FSATcpServer::Retire(FSAConnection* Conn)
{
    FScopeLock L(&ActiveLock);
    for (int32 i = Active.Num() - 1; i >= 0; --i)
    {
        if (Active[i].Conn == Conn) { Active.RemoveAtSwap(i); break; }
    }
}

void FSATcpServer::Stop()
{
    if (!bRunning.exchange(false)) return;
    if (Listener) { Listener->Stop(); Listener.Reset(); }

    TArray<FActive> Snapshot;
    { FScopeLock L(&ActiveLock); Snapshot = Active; Active.Empty(); }

    for (auto& A : Snapshot)
    {
        A.Conn->RequestStop();
        if (A.Thread) { A.Thread->WaitForCompletion(); delete A.Thread; }
        delete A.Conn;
    }
    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: raw TCP transport stopped"));
}

int32 FSATcpServer::GetActiveConnectionCount() const
{
    FScopeLock L(&ActiveLock);
    return Active.Num();
}

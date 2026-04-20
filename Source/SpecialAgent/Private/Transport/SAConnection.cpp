#include "Transport/SAConnection.h"
#include "Transport/SAHttpParser.h"
#include "Transport/SATransportConstants.h"
#include "Transport/SATcpServer.h"
#include "Transport/SASessionRegistry.h"
#include "MCPServer.h"
#include "MCPRequestRouter.h"
#include "MCPCommon/MCPRequestContext.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

FSAConnection::FSAConnection(FSATcpServer* InOwner, FSocket* InSocket,
                             TSharedPtr<FMCPRequestRouter> InRouter)
    : Owner(InOwner), Socket(InSocket), Router(InRouter), Writer(InSocket) {}

FSAConnection::~FSAConnection()
{
    if (Socket)
    {
        if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
        {
            SS->DestroySocket(Socket);
        }
        Socket = nullptr;
    }
}

// Phase 1: block until full request received or fatal error.
bool FSAConnection::ReadFullRequest(TArray<uint8>& Buf, FSAHttpRequest& Req)
{
    const double Deadline = FPlatformTime::Seconds() + SATransport::IdleReadTimeoutSeconds;
    uint8 Chunk[4096];
    while (!bStopping)
    {
        if (FPlatformTime::Seconds() > Deadline) return false;

        uint32 Pending = 0;
        if (!Socket->HasPendingData(Pending))
        {
            FPlatformProcess::Sleep(SATransport::SocketPollMilliseconds / 1000.0f);
            continue;
        }

        int32 Read = 0;
        if (!Socket->Recv(Chunk, sizeof(Chunk), Read)) return false;
        if (Read <= 0) continue;
        Buf.Append(Chunk, Read);
        if (Buf.Num() > SATransport::MaxHeaderBytes + SATransport::MaxBodyBytes) return false;

        int32 Consumed = 0;
        const auto R = SAHttpParser::Parse(Buf, Req, Consumed);
        switch (R)
        {
            case ESAHttpParseResult::Success:            return true;
            case ESAHttpParseResult::Incomplete:         continue;
            case ESAHttpParseResult::HeadersTooLarge:
                Writer.WriteSingleBodyString(431, TEXT("text/plain"), TEXT("headers too large"));
                return false;
            case ESAHttpParseResult::BodyTooLarge:
                Writer.WriteSingleBodyString(413, TEXT("text/plain"), TEXT("payload too large"));
                return false;
            case ESAHttpParseResult::LengthRequired:
                Writer.WriteSingleBodyString(411, TEXT("text/plain"), TEXT("content-length required"));
                return false;
            case ESAHttpParseResult::ChunkedUnsupported:
                Writer.WriteSingleBodyString(501, TEXT("text/plain"),
                    TEXT("chunked request bodies unsupported"));
                return false;
            case ESAHttpParseResult::MethodUnsupported:
                Writer.WriteSingleBodyString(405, TEXT("text/plain"), TEXT("method not allowed"));
                return false;
            default:
                Writer.WriteSingleBodyString(400, TEXT("text/plain"), TEXT("bad request"));
                return false;
        }
    }
    return false;
}

void FSAConnection::HandleGetHealth()
{
    const FString Body = FString::Printf(
        TEXT("{\"status\":\"healthy\",\"server\":\"SpecialAgent MCP Server\","
             "\"version\":\"1.0.0\",\"port\":%d,\"running\":true}"),
        8767);
    Writer.WriteSingleBodyString(200, TEXT("application/json"), Body);
}

void FSAConnection::HandleOptions()
{
    TMap<FString,FString> H;
    H.Add(TEXT("Access-Control-Allow-Methods"), TEXT("GET, POST, OPTIONS"));
    H.Add(TEXT("Access-Control-Allow-Headers"),
          TEXT("Content-Type, Accept, Authorization, Mcp-Session-Id"));
    H.Add(TEXT("Access-Control-Max-Age"), TEXT("86400"));
    Writer.WriteSingleBody(204, TEXT("text/plain"), TConstArrayView<uint8>{}, H);
}

void FSAConnection::HandleNotFound()
{
    Writer.WriteSingleBodyString(404, TEXT("application/json"),
        TEXT("{\"error\":\"not found\"}"));
}

void FSAConnection::HandlePostMCP(const FSAHttpRequest& Req)
{
    // Parse JSON-RPC (body must already be fully read).
    // Phase 1: synchronous dispatch on a background task (matches today's
    // post-threading-fix behaviour). Context is empty for now — session
    // wiring lands in Phase 3.
    const FString BodyStr = FString::ConstructFromPtrSize(
        UTF8_TO_TCHAR((const ANSICHAR*)Req.Body.GetData()), Req.Body.Num());

    FMCPRequest Msg;
    if (!FSpecialAgentMCPServer::ParseRequest(BodyStr, Msg))
    {
        // JSON-RPC parse error as a JSON body (matches today).
        Writer.WriteSingleBodyString(200, TEXT("application/json"),
            FSpecialAgentMCPServer::FormatResponse(
                FMCPResponse::Error(TEXT(""), -32700, TEXT("Parse error: Invalid JSON"))));
        return;
    }

    // Dispatch on a background thread so game-thread dispatcher can safely block.
    TPromise<FString> P;
    auto Fut = P.GetFuture();
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [Router = Router, Msg = MoveTemp(Msg), Promise = MoveTemp(P)]() mutable
        {
            FMCPRequestContext Ctx;   // Phase 3 fills this in
            FMCPResponse R = Router->RouteRequest(Msg, Ctx);
            Promise.SetValue(FSpecialAgentMCPServer::FormatResponse(R));
        });
    const FString Json = Fut.Get();
    Writer.WriteSingleBodyString(200, TEXT("application/json"), Json);
}

uint32 FSAConnection::Run()
{
    TArray<uint8> Buf;
    FSAHttpRequest Req;
    if (ReadFullRequest(Buf, Req))
    {
        if (Req.Verb == TEXT("OPTIONS"))
        {
            HandleOptions();
        }
        else if (Req.Verb == TEXT("GET") && Req.Path == TEXT("/health"))
        {
            HandleGetHealth();
        }
        else if (Req.Verb == TEXT("GET") && Req.Path == TEXT("/sse"))
        {
            // Phase 3b — for Phase 1 we return 501 so the path is reachable but stubbed.
            Writer.WriteSingleBodyString(501, TEXT("text/plain"), TEXT("SSE GET not yet implemented"));
        }
        else if (Req.Verb == TEXT("POST") && Req.Path == TEXT("/mcp"))
        {
            HandlePostMCP(Req);
        }
        else
        {
            HandleNotFound();
        }
    }
    if (Owner) Owner->Retire(this);
    return 0;
}

void FSAConnection::OnReplacedBySessionRegistry()
{
    Writer.WriteKeepAlive();  // best-effort ": keepalive\n\n"
    Writer.Finish();
    bStopping = true;
}

bool FSAConnection::PushSSEEvent(const FString& EventName, const FString& Data)
{
    return Writer.WriteEvent(EventName, Data);
}

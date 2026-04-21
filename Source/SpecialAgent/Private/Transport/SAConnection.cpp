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

namespace
{
    FMCPRequestContext BuildContextFromRequest(const FSAHttpRequest& Req, const FMCPRequest& Msg)
    {
        FMCPRequestContext Ctx;
        Ctx.SessionId = Req.GetHeader(TEXT("Mcp-Session-Id"));

        // Extract progressToken from params._meta.progressToken when present.
        if (Msg.Params.IsValid())
        {
            const TSharedPtr<FJsonObject>* MetaObj = nullptr;
            if (Msg.Params->TryGetObjectField(TEXT("_meta"), MetaObj))
            {
                Ctx.ProgressToken = (*MetaObj)->TryGetField(TEXT("progressToken"));
            }
        }

        // Real SendProgress: serialise notifications/progress JSON-RPC message
        // and route to the session's registered SSE stream via the registry.
        // No-op if the session has no active GET /sse stream — handlers may
        // always call this without checking.
        const FString SessionIdCopy = Ctx.SessionId;
        const TSharedPtr<FJsonValue> TokenCopy = Ctx.ProgressToken;
        Ctx.SendProgress = [SessionIdCopy, TokenCopy](double Progress, double Total, const FString& Message)
        {
            if (SessionIdCopy.IsEmpty()) return;

            TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
            Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
            Notification->SetStringField(TEXT("method"), TEXT("notifications/progress"));

            TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
            if (TokenCopy.IsValid())
            {
                Params->SetField(TEXT("progressToken"), TokenCopy);
            }
            Params->SetNumberField(TEXT("progress"), Progress);
            Params->SetNumberField(TEXT("total"),    Total);
            if (!Message.IsEmpty())
            {
                Params->SetStringField(TEXT("message"), Message);
            }
            Notification->SetObjectField(TEXT("params"), Params);

            FSASessionRegistry::Get().SendNotification(SessionIdCopy, Notification);
        };
        return Ctx;
    }
}

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
    if (Req.AcceptContains(TEXT("text/event-stream")))
    {
        HandlePostMCPSSE(Req);
        return;
    }

    // Parse JSON-RPC. Req.Body is a raw byte buffer — convert it to FString
    // via an explicit-length UTF-8 converter. Do NOT use UTF8_TO_TCHAR with
    // ConstructFromPtrSize + byte count: UTF8_TO_TCHAR expects a null-
    // terminated input and ConstructFromPtrSize's length is in TCHARs,
    // so the old form read past the buffer and passed a byte count as a
    // TCHAR count, producing corrupted JSON that failed to parse.
    FUTF8ToTCHAR Converter((const ANSICHAR*)Req.Body.GetData(), Req.Body.Num());
    const FString BodyStr(Converter.Length(), Converter.Get());

    FMCPRequest Msg;
    if (!FSpecialAgentMCPServer::ParseRequest(BodyStr, Msg))
    {
        // JSON-RPC parse error as a JSON body (matches today).
        Writer.WriteSingleBodyString(200, TEXT("application/json"),
            FSpecialAgentMCPServer::FormatResponse(
                FMCPResponse::Error(TEXT(""), -32700, TEXT("Parse error: Invalid JSON"))));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: POST /mcp (single-body) method=%s"), *Msg.Method);

    // Session id machinery: mint on initialize, validate otherwise.
    FString MintedSessionId;  // non-empty only when we minted one for 'initialize'
    if (Msg.Method == TEXT("initialize"))
    {
        MintedSessionId = FSASessionRegistry::Get().MintSession();
    }
    else
    {
        const FString Sid = Req.GetHeader(TEXT("Mcp-Session-Id"));
        if (Sid.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: 400 — missing Mcp-Session-Id on method=%s"), *Msg.Method);
            Writer.WriteSingleBodyString(400, TEXT("application/json"),
                TEXT("{\"error\":\"missing Mcp-Session-Id\"}"));
            return;
        }
        if (!FSASessionRegistry::Get().IsSessionValid(Sid))
        {
            // Permissive: adopt the client-asserted id instead of forcing
            // a reconnect. Survives editor restart (registry is in-memory)
            // without Claude Code stuck on a stale cached session id.
            if (FSASessionRegistry::Get().AdoptSession(Sid))
            {
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: adopted client session %s on method=%s"), *Sid, *Msg.Method);
            }
        }
    }

    FMCPRequestContext Ctx = BuildContextFromRequest(Req, Msg);
    if (!MintedSessionId.IsEmpty()) Ctx.SessionId = MintedSessionId;

    // Dispatch on a background thread so game-thread dispatcher can safely block.
    TPromise<FString> P;
    auto Fut = P.GetFuture();
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [Router = Router, Msg = MoveTemp(Msg), Ctx = MoveTemp(Ctx), Promise = MoveTemp(P)]() mutable
        {
            FMCPResponse R = Router->RouteRequest(Msg, Ctx);
            Promise.SetValue(FSpecialAgentMCPServer::FormatResponse(R));
        });
    const FString Json = Fut.Get();

    TMap<FString, FString> ExtraHeaders;
    if (!MintedSessionId.IsEmpty()) ExtraHeaders.Add(TEXT("Mcp-Session-Id"), MintedSessionId);
    Writer.WriteSingleBodyString(200, TEXT("application/json"), Json, ExtraHeaders);
}

void FSAConnection::HandlePostMCPSSE(const FSAHttpRequest& Req)
{
    // Parse up front so we can emit a parse-error event in SSE mode too.
    // See HandlePostMCP for why we must use an explicit-length UTF-8 converter.
    FUTF8ToTCHAR Converter((const ANSICHAR*)Req.Body.GetData(), Req.Body.Num());
    const FString BodyStr(Converter.Length(), Converter.Get());

    FMCPRequest Msg;
    const bool bParsedOk = FSpecialAgentMCPServer::ParseRequest(BodyStr, Msg);

    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: POST /mcp (SSE) method=%s parsed=%d"),
        bParsedOk ? *Msg.Method : TEXT("<unparsed>"),
        bParsedOk ? 1 : 0);

    // Session id machinery: mint on initialize, validate otherwise.
    // Must happen BEFORE BeginSSE so we can still send HTTP error status codes.
    FString MintedSessionId;  // non-empty only when we minted one for 'initialize'
    if (bParsedOk)
    {
        if (Msg.Method == TEXT("initialize"))
        {
            MintedSessionId = FSASessionRegistry::Get().MintSession();
        }
        else
        {
            const FString Sid = Req.GetHeader(TEXT("Mcp-Session-Id"));
            if (Sid.IsEmpty())
            {
                UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: SSE 400 — missing Mcp-Session-Id on method=%s"), *Msg.Method);
                Writer.WriteSingleBodyString(400, TEXT("application/json"),
                    TEXT("{\"error\":\"missing Mcp-Session-Id\"}"));
                return;
            }
            if (!FSASessionRegistry::Get().IsSessionValid(Sid))
            {
                if (FSASessionRegistry::Get().AdoptSession(Sid))
                {
                    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: SSE adopted client session %s on method=%s"), *Sid, *Msg.Method);
                }
            }
        }
    }

    TMap<FString, FString> ExtraHeaders;
    if (!MintedSessionId.IsEmpty()) ExtraHeaders.Add(TEXT("Mcp-Session-Id"), MintedSessionId);
    if (!Writer.BeginSSE(ExtraHeaders)) return;

    // Prime the stream so clients with short read-idle timeouts (e.g. Claude
    // Code / undici) see at least one chunk before the handler starts working.
    // Without this, a ~5 s idle timeout kills the stream before our ticker
    // fires its first keep-alive.
    Writer.WriteKeepAlive();

    if (!bParsedOk)
    {
        Writer.WriteEvent(TEXT("message"), FSpecialAgentMCPServer::FormatResponse(
            FMCPResponse::Error(TEXT(""), -32700, TEXT("Parse error: Invalid JSON"))));
        Writer.Finish();
        return;
    }

    FMCPRequestContext Ctx = BuildContextFromRequest(Req, Msg);
    if (!MintedSessionId.IsEmpty()) Ctx.SessionId = MintedSessionId;

    // Dispatch on background thread. The current connection thread stays on
    // the socket to flush keep-alives while the router runs, preventing any
    // client-side HTTP read timeout from firing during long handlers (e.g.
    // material/* normal-map compute, asset imports, PIE start).
    std::atomic<bool> bDone{ false };
    TPromise<FString> P;
    auto Fut = P.GetFuture();
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [Router = Router, Msg, Ctx = MoveTemp(Ctx), &bDone, Promise = MoveTemp(P)]() mutable
        {
            FMCPResponse R = Router->RouteRequest(Msg, Ctx);
            Promise.SetValue(FSpecialAgentMCPServer::FormatResponse(R));
            bDone.store(true, std::memory_order_release);
        });

    // Keep-alive ticker. Wait up to kKeepAliveIntervalSeconds in 100 ms slices
    // so we respond to bStopping / socket death within one slice. When the
    // slice elapses without completion, flush a ": keepalive\n\n" frame.
    // NOTE: capturing &bDone is safe because Fut.Get() below blocks this
    // function until the background task has run.
    while (!bDone.load(std::memory_order_acquire) && !bStopping && !Writer.IsDead())
    {
        for (int32 i = 0;
             i < SATransport::KeepAliveIntervalSeconds * 10
                 && !bDone.load(std::memory_order_acquire)
                 && !bStopping
                 && !Writer.IsDead();
             ++i)
        {
            FPlatformProcess::Sleep(0.1f);
        }
        if (!bDone.load(std::memory_order_acquire) && !bStopping && !Writer.IsDead())
        {
            Writer.WriteKeepAlive();
        }
    }

    if (!Writer.IsDead())
    {
        const FString Json = Fut.Get();
        Writer.WriteEvent(TEXT("message"), Json);
        Writer.Finish();
    }
}

void FSAConnection::HandleGetSSE(const FSAHttpRequest& Req)
{
    const FString Sid = Req.GetHeader(TEXT("Mcp-Session-Id"));
    if (Sid.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: GET /sse 400 — missing Mcp-Session-Id"));
        Writer.WriteSingleBodyString(400, TEXT("application/json"),
            TEXT("{\"error\":\"missing Mcp-Session-Id\"}"));
        return;
    }
    if (!FSASessionRegistry::Get().IsSessionValid(Sid))
    {
        if (FSASessionRegistry::Get().AdoptSession(Sid))
        {
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: GET /sse adopted client session %s"), *Sid);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: GET /sse session=%s opening stream"), *Sid);
    if (!Writer.BeginSSE()) return;

    // Prime the stream so strict HTTP clients see a chunk immediately.
    Writer.WriteKeepAlive();

    ActiveSessionIdForStream = Sid;
    FSASessionRegistry::Get().RegisterStream(Sid, this);

    // Keep-alive loop. Runs on this FRunnable until bStopping or socket death.
    // Notifications pushed via FSASessionRegistry::SendNotification -> PushSSEEvent
    // interleave with keep-alives safely under Writer's SSEWriteLock.
    while (!bStopping && !Writer.IsDead())
    {
        for (int32 i = 0;
             i < SATransport::KeepAliveIntervalSeconds * 10
                 && !bStopping
                 && !Writer.IsDead();
             ++i)
        {
            FPlatformProcess::Sleep(0.1f);
        }
        if (!bStopping && !Writer.IsDead())
        {
            Writer.WriteKeepAlive();
        }
    }

    FSASessionRegistry::Get().UnregisterStream(Sid, this);
    ActiveSessionIdForStream.Empty();
    Writer.Finish();
}

uint32 FSAConnection::Run()
{
    TArray<uint8> Buf;
    FSAHttpRequest Req;
    if (ReadFullRequest(Buf, Req))
    {
        const FString Sid = Req.GetHeader(TEXT("Mcp-Session-Id"));
        UE_LOG(LogTemp, Log,
            TEXT("SpecialAgent: %s %s Accept=%s Session=%s body=%dB"),
            *Req.Verb, *Req.Path,
            *Req.GetHeader(TEXT("Accept")),
            Sid.IsEmpty() ? TEXT("<none>") : *Sid,
            Req.Body.Num());

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
            HandleGetSSE(Req);
        }
        else if (Req.Verb == TEXT("POST") && Req.Path == TEXT("/mcp"))
        {
            HandlePostMCP(Req);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: 404 for %s %s"), *Req.Verb, *Req.Path);
            HandleNotFound();
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: request read failed or timed out"));
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

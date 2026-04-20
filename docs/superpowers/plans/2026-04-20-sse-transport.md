# SSE Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace UE's `FHttpServerModule` with a raw TCP transport that supports real SSE, so Claude Code's long-running tool calls (material normals, asset import, PIE start) do not time out, and so the server can push `notifications/progress` events to a persistent `GET /sse` stream.

**Architecture:** Phased hard cutover. A new `Transport/` module under `Source/SpecialAgent/Private/` implements HTTP/1.1 over raw `FTcpListener` + `FSocket`. One `FRunnable` worker thread per connection. `IMCPService::HandleRequest` grows a `const FMCPRequestContext&` parameter so handlers can emit progress. A `FSASessionRegistry` maps `Mcp-Session-Id` to the active SSE stream. After cut-over, `HTTPServer` module dependency is removed.

**Tech Stack:** Unreal Engine 5.7, C++, `Sockets` + `Networking` modules, `FTcpListener`, `FSocket`, `FRunnable`, `FRWLock`, `FCriticalSection`, `FJson`, `UE_LOG`.

**Spec:** See `docs/superpowers/specs/2026-04-20-sse-transport-design.md`.

**Branch / worktree:** `feature/sse-transport` at `.worktrees/sse-transport/` (from the plugin root).

---

## UE-Specific TDD Note

UE plugin compiles are slow (minutes, not seconds), and tests need the editor running. The plan replaces the usual tight red-green loop with:

- **Unit-testable pure functions** (HTTP parser): real UE Automation tests, run via `Session Frontend → Automation → Run Tests` or `UnrealEditor -ExecCmds="Automation RunTests SpecialAgent"`.
- **Integration testable via `curl`**: a smoke script committed to the repo (`docs/superpowers/specs/sse-transport-smoke.sh`) that exercises the live server.
- **Each task ends with a commit** once the code compiles; full-build verification happens at phase boundaries (Task 1.11, 2.6, 3b.5, 4.4).

Because of build cost, tasks are grouped so each parallel agent's work compiles independently before merging. The 45-file `HandleRequest` signature bump is the one step that *must* land together — handled by a fan-out in Task 1.9.

---

## File structure

### New files (all in worktree)

```
Source/SpecialAgent/Public/MCPCommon/
    MCPRequestContext.h               — FMCPRequestContext (session id, progress token, SendProgress)

Source/SpecialAgent/Private/Transport/
    SATransportConstants.h            — kKeepAliveIntervalSeconds, kMaxConnections, limits
    SAHttpParser.h / .cpp             — pure HTTP/1.1 request parser
    SAHttpResponse.h / .cpp           — response writer (single-body + chunked SSE)
    SASessionRegistry.h / .cpp        — session id → connection map (FRWLock)
    SAConnection.h / .cpp             — per-connection FRunnable: parse → route → write
    SATcpServer.h / .cpp              — FTcpListener + active-connection registry

Source/SpecialAgent/Private/Tests/
    SAHttpParserTests.cpp             — UE Automation tests for parser

docs/superpowers/specs/
    sse-transport-smoke.sh            — curl-based integration smoke script
```

### Modified files

```
Source/SpecialAgent/Public/Services/IMCPService.h        — HandleRequest signature + FMCPToolInfo unchanged
Source/SpecialAgent/Private/MCPServer.h                  — thin wrapper; SSE dead-code removed
Source/SpecialAgent/Private/MCPServer.cpp                — owns FSATcpServer; HandleMessage / HandleSSEConnection / HandleCORS / HandleHealth deleted
Source/SpecialAgent/Private/MCPRequestRouter.h / .cpp    — RouteRequest takes FMCPRequestContext&
Source/SpecialAgent/Private/Services/*.cpp               — 45 files: HandleRequest(..., const FMCPRequestContext& Ctx)
Source/SpecialAgent/Public/Services/*.h                  — 45 headers: signature parity
Source/SpecialAgent/SpecialAgent.Build.cs                — remove "HTTPServer" (Phase 4)
```

### No-change files

- All `FMCPToolBuilder` / `FMCPJson` / `FMCPActorResolver` code.
- `FGameThreadDispatcher` and `FMCPGameThreadProcessor` — contract unchanged.
- Service implementation bodies (apart from the mechanical signature bump).

---

## Phase 0 — Setup

### Task 0.1: Add transport constants header

**Files:**
- Create: `Source/SpecialAgent/Private/Transport/SATransportConstants.h`

- [ ] **Step 1: Create the constants header**

```cpp
#pragma once

#include "CoreMinimal.h"

namespace SATransport
{
    // SSE keep-alive frame cadence. Set conservatively below typical client
    // HTTP-read timeouts (30–60 s). Bumping this also bumps the worst-case
    // editor-shutdown wait.
    constexpr int32 KeepAliveIntervalSeconds = 15;

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
```

- [ ] **Step 2: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SATransportConstants.h
git commit -m "feat(SpecialAgent/transport): add SATransportConstants.h with named tuning parameters"
```

### Task 0.2: Create FMCPRequestContext

**Files:**
- Create: `Source/SpecialAgent/Public/MCPCommon/MCPRequestContext.h`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"

/**
 * Request-scoped context passed into every IMCPService::HandleRequest call.
 *
 * Carries:
 *  - SessionId:      the Mcp-Session-Id minted during 'initialize'; empty for
 *                    the initialize request itself.
 *  - ProgressToken:  extracted from params._meta.progressToken (may be null).
 *  - SendProgress:   closure a handler may call to push a notifications/progress
 *                    event to the session's SSE stream. Always safe to call;
 *                    no-op when the session has no registered SSE stream.
 */
struct FMCPRequestContext
{
    FString SessionId;
    TSharedPtr<FJsonValue> ProgressToken;
    TFunction<void(double Progress, double Total, const FString& Message)> SendProgress;
};
```

- [ ] **Step 2: Commit**

```bash
git add Source/SpecialAgent/Public/MCPCommon/MCPRequestContext.h
git commit -m "feat(SpecialAgent/transport): add FMCPRequestContext for request-scoped progress plumbing"
```

---

## Phase 1 — Raw HTTP transport parity

### Task 1.1: Implement FSAHttpParser + unit tests (PARALLEL-1a)

**Files:**
- Create: `Source/SpecialAgent/Private/Transport/SAHttpParser.h`
- Create: `Source/SpecialAgent/Private/Transport/SAHttpParser.cpp`
- Create: `Source/SpecialAgent/Private/Tests/SAHttpParserTests.cpp`

- [ ] **Step 1: Write the parser header**

```cpp
#pragma once
#include "CoreMinimal.h"

enum class ESAHttpParseResult : uint8
{
    Success,
    Incomplete,       // buffer doesn't yet contain a full request
    BadRequest,       // malformed request line / headers
    HeadersTooLarge,
    BodyTooLarge,
    LengthRequired,   // POST body without Content-Length
    ChunkedUnsupported,
    MethodUnsupported,
    VersionUnsupported,
};

struct FSAHttpRequest
{
    FString Verb;     // "GET" | "POST" | "OPTIONS"
    FString Path;     // "/mcp", "/sse", "/health"
    FString Query;    // everything after '?' (empty if none)
    TMap<FString, FString> Headers;  // case-insensitive lookup via GetHeader
    TArray<uint8> Body;

    FString GetHeader(const FString& Name) const;          // case-insensitive
    int32 GetContentLength() const;                        // parsed or -1
    bool AcceptContains(const FString& MediaType) const;   // Accept header scan
};

namespace SAHttpParser
{
    /** Attempt to parse one HTTP/1.1 request out of Buffer. On Success, OutRequest
     *  is populated and OutBytesConsumed is the number of bytes read (may be less
     *  than Buffer.Num() for pipelined-looking input — we still only consume one). */
    ESAHttpParseResult Parse(TConstArrayView<uint8> Buffer,
                             FSAHttpRequest& OutRequest,
                             int32& OutBytesConsumed);
}
```

- [ ] **Step 2: Write the unit test file**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Transport/SAHttpParser.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSAHttpParserGoodGet,
    "SpecialAgent.Transport.Parser.GoodGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSAHttpParserGoodGet::RunTest(const FString& /*Parameters*/)
{
    const char* Raw =
        "GET /health HTTP/1.1\r\n"
        "Host: 127.0.0.1:8767\r\n"
        "Accept: application/json\r\n"
        "\r\n";
    TArray<uint8> Buf((uint8*)Raw, FCStringAnsi::Strlen(Raw));
    FSAHttpRequest Req;
    int32 Consumed = 0;
    const auto R = SAHttpParser::Parse(Buf, Req, Consumed);
    TestEqual(TEXT("Parse result"), (int32)R, (int32)ESAHttpParseResult::Success);
    TestEqual(TEXT("Verb"), Req.Verb, FString(TEXT("GET")));
    TestEqual(TEXT("Path"), Req.Path, FString(TEXT("/health")));
    TestEqual(TEXT("Accept header"), Req.GetHeader(TEXT("accept")), FString(TEXT("application/json")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSAHttpParserPostWithBody,
    "SpecialAgent.Transport.Parser.PostWithBody",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSAHttpParserPostWithBody::RunTest(const FString&)
{
    const char* Raw =
        "POST /mcp HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 17\r\n"
        "\r\n"
        "{\"jsonrpc\":\"2.0\"}";
    TArray<uint8> Buf((uint8*)Raw, FCStringAnsi::Strlen(Raw));
    FSAHttpRequest Req; int32 Consumed = 0;
    TestEqual(TEXT("Parse result"),
        (int32)SAHttpParser::Parse(Buf, Req, Consumed),
        (int32)ESAHttpParseResult::Success);
    TestEqual(TEXT("Body length"), Req.Body.Num(), 17);
    TestEqual(TEXT("Content-Length"), Req.GetContentLength(), 17);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSAHttpParserIncomplete,
    "SpecialAgent.Transport.Parser.Incomplete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSAHttpParserIncomplete::RunTest(const FString&)
{
    const char* Raw = "GET /health HTTP";  // cut mid-line
    TArray<uint8> Buf((uint8*)Raw, FCStringAnsi::Strlen(Raw));
    FSAHttpRequest Req; int32 Consumed = 0;
    TestEqual(TEXT("Parse result"),
        (int32)SAHttpParser::Parse(Buf, Req, Consumed),
        (int32)ESAHttpParseResult::Incomplete);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSAHttpParserChunkedRequestRejected,
    "SpecialAgent.Transport.Parser.ChunkedRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSAHttpParserChunkedRequestRejected::RunTest(const FString&)
{
    const char* Raw =
        "POST /mcp HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    TArray<uint8> Buf((uint8*)Raw, FCStringAnsi::Strlen(Raw));
    FSAHttpRequest Req; int32 Consumed = 0;
    TestEqual(TEXT("Parse result"),
        (int32)SAHttpParser::Parse(Buf, Req, Consumed),
        (int32)ESAHttpParseResult::ChunkedUnsupported);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSAHttpParserHeadersTooLarge,
    "SpecialAgent.Transport.Parser.HeadersTooLarge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSAHttpParserHeadersTooLarge::RunTest(const FString&)
{
    FString BigHeader = FString::ChrN(17 * 1024, TEXT('x'));
    FString Raw = FString::Printf(TEXT("GET /health HTTP/1.1\r\nX-Big: %s\r\n\r\n"), *BigHeader);
    FTCHARToUTF8 Utf8(*Raw);
    TArray<uint8> Buf((uint8*)Utf8.Get(), Utf8.Length());
    FSAHttpRequest Req; int32 Consumed = 0;
    TestEqual(TEXT("Parse result"),
        (int32)SAHttpParser::Parse(Buf, Req, Consumed),
        (int32)ESAHttpParseResult::HeadersTooLarge);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Write the parser implementation**

```cpp
// SAHttpParser.cpp
#include "Transport/SAHttpParser.h"
#include "Transport/SATransportConstants.h"
#include "Containers/UnrealString.h"

FString FSAHttpRequest::GetHeader(const FString& Name) const
{
    for (const auto& KV : Headers)
    {
        if (KV.Key.Equals(Name, ESearchCase::IgnoreCase)) return KV.Value;
    }
    return FString();
}

int32 FSAHttpRequest::GetContentLength() const
{
    FString V = GetHeader(TEXT("Content-Length"));
    if (V.IsEmpty()) return -1;
    return FCString::Atoi(*V);
}

bool FSAHttpRequest::AcceptContains(const FString& MediaType) const
{
    return GetHeader(TEXT("Accept")).Contains(MediaType, ESearchCase::IgnoreCase);
}

static int32 FindCRLFCRLF(TConstArrayView<uint8> Buf)
{
    for (int32 i = 0; i + 3 < Buf.Num(); ++i)
    {
        if (Buf[i] == '\r' && Buf[i+1] == '\n' && Buf[i+2] == '\r' && Buf[i+3] == '\n')
            return i;
    }
    return INDEX_NONE;
}

static FString BufToFString(TConstArrayView<uint8> Buf)
{
    // Headers are ASCII. Safe to bring in as UTF-8; non-ASCII in headers is pathological.
    return FString::ConstructFromPtrSize(UTF8_TO_TCHAR((const ANSICHAR*)Buf.GetData()), Buf.Num());
}

namespace SAHttpParser
{
    ESAHttpParseResult Parse(TConstArrayView<uint8> Buffer,
                             FSAHttpRequest& Out,
                             int32& OutBytesConsumed)
    {
        OutBytesConsumed = 0;
        if (Buffer.Num() == 0) return ESAHttpParseResult::Incomplete;

        const int32 HeadersEnd = FindCRLFCRLF(Buffer);
        if (HeadersEnd == INDEX_NONE)
        {
            if (Buffer.Num() > SATransport::MaxHeaderBytes)
                return ESAHttpParseResult::HeadersTooLarge;
            return ESAHttpParseResult::Incomplete;
        }
        if (HeadersEnd > SATransport::MaxHeaderBytes)
            return ESAHttpParseResult::HeadersTooLarge;

        FString Head = BufToFString(Buffer.Slice(0, HeadersEnd));
        TArray<FString> Lines;
        Head.ParseIntoArray(Lines, TEXT("\r\n"), /*CullEmpty=*/true);
        if (Lines.Num() == 0) return ESAHttpParseResult::BadRequest;

        // Request line: VERB SP PATH[?QUERY] SP HTTP/1.1
        TArray<FString> RequestLineParts;
        Lines[0].ParseIntoArray(RequestLineParts, TEXT(" "), /*CullEmpty=*/true);
        if (RequestLineParts.Num() != 3) return ESAHttpParseResult::BadRequest;

        Out.Verb = RequestLineParts[0];
        if (Out.Verb != TEXT("GET") && Out.Verb != TEXT("POST") && Out.Verb != TEXT("OPTIONS"))
            return ESAHttpParseResult::MethodUnsupported;

        const FString Target = RequestLineParts[1];
        int32 QIdx = INDEX_NONE;
        if (Target.FindChar('?', QIdx))
        {
            Out.Path  = Target.Left(QIdx);
            Out.Query = Target.Mid(QIdx + 1);
        }
        else
        {
            Out.Path = Target;
        }

        if (!RequestLineParts[2].StartsWith(TEXT("HTTP/1.1"), ESearchCase::CaseSensitive))
            return ESAHttpParseResult::VersionUnsupported;

        // Headers
        for (int32 i = 1; i < Lines.Num(); ++i)
        {
            int32 Colon = INDEX_NONE;
            if (!Lines[i].FindChar(':', Colon)) return ESAHttpParseResult::BadRequest;
            FString Key = Lines[i].Left(Colon).TrimStartAndEnd();
            FString Val = Lines[i].Mid(Colon + 1).TrimStartAndEnd();
            Out.Headers.Add(Key, Val);
        }

        // Reject Transfer-Encoding: chunked (request body) — v1 unsupported.
        if (Out.GetHeader(TEXT("Transfer-Encoding")).Contains(TEXT("chunked"), ESearchCase::IgnoreCase))
            return ESAHttpParseResult::ChunkedUnsupported;

        const int32 BodyStart = HeadersEnd + 4;
        const int32 ContentLength = Out.GetContentLength();

        if (Out.Verb == TEXT("POST"))
        {
            if (ContentLength < 0) return ESAHttpParseResult::LengthRequired;
            if (ContentLength > SATransport::MaxBodyBytes) return ESAHttpParseResult::BodyTooLarge;
            if (Buffer.Num() < BodyStart + ContentLength) return ESAHttpParseResult::Incomplete;
            Out.Body.Append(&Buffer[BodyStart], ContentLength);
            OutBytesConsumed = BodyStart + ContentLength;
        }
        else
        {
            // GET/OPTIONS: no body expected.
            OutBytesConsumed = BodyStart;
        }

        return ESAHttpParseResult::Success;
    }
}
```

- [ ] **Step 4: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAHttpParser.h \
        Source/SpecialAgent/Private/Transport/SAHttpParser.cpp \
        Source/SpecialAgent/Private/Tests/SAHttpParserTests.cpp
git commit -m "feat(SpecialAgent/transport): add HTTP/1.1 request parser with automation tests"
```

### Task 1.2: Implement FSAHttpResponse (single-body mode only) (PARALLEL-1b)

**Files:**
- Create: `Source/SpecialAgent/Private/Transport/SAHttpResponse.h`
- Create: `Source/SpecialAgent/Private/Transport/SAHttpResponse.cpp`

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class FSocket;

class FSAHttpResponse
{
public:
    explicit FSAHttpResponse(FSocket* InSocket);

    /** Single-body mode: write status line + headers + body + close. */
    bool WriteSingleBody(int32 StatusCode,
                         const FString& ContentType,
                         TConstArrayView<uint8> Body,
                         const TMap<FString, FString>& ExtraHeaders = {});

    bool WriteSingleBodyString(int32 StatusCode,
                               const FString& ContentType,
                               const FString& Body,
                               const TMap<FString, FString>& ExtraHeaders = {});

    /** Chunked-SSE mode. Call BeginSSE once, then WriteEvent/WriteKeepAlive
     *  as often as needed, then Finish. All three are thread-safe: they
     *  acquire SSEWriteLock internally. */
    bool BeginSSE(const TMap<FString, FString>& ExtraHeaders = {});
    bool WriteEvent(const FString& EventName, const FString& Data);
    bool WriteKeepAlive();
    bool Finish();

    bool IsDead() const { return bDead.load(std::memory_order_acquire); }

private:
    FSocket* Socket;
    FCriticalSection SSEWriteLock;
    std::atomic<bool> bDead { false };

    bool WriteAll(const FString& Utf8Payload);
    bool WriteAllRaw(TConstArrayView<uint8> Payload);
    static FString StatusText(int32 Code);
};
```

- [ ] **Step 2: Write the implementation**

```cpp
#include "Transport/SAHttpResponse.h"
#include "Sockets.h"

FSAHttpResponse::FSAHttpResponse(FSocket* InSocket) : Socket(InSocket) {}

FString FSAHttpResponse::StatusText(int32 Code)
{
    switch (Code)
    {
        case 200: return TEXT("OK");
        case 204: return TEXT("No Content");
        case 400: return TEXT("Bad Request");
        case 404: return TEXT("Not Found");
        case 405: return TEXT("Method Not Allowed");
        case 411: return TEXT("Length Required");
        case 413: return TEXT("Payload Too Large");
        case 431: return TEXT("Request Header Fields Too Large");
        case 501: return TEXT("Not Implemented");
        case 503: return TEXT("Service Unavailable");
        default:  return TEXT("Unknown");
    }
}

bool FSAHttpResponse::WriteAll(const FString& Payload)
{
    FTCHARToUTF8 Utf8(*Payload);
    return WriteAllRaw(TConstArrayView<uint8>((const uint8*)Utf8.Get(), Utf8.Length()));
}

bool FSAHttpResponse::WriteAllRaw(TConstArrayView<uint8> Payload)
{
    int32 Offset = 0;
    while (Offset < Payload.Num())
    {
        int32 Sent = 0;
        if (!Socket || !Socket->Send(Payload.GetData() + Offset, Payload.Num() - Offset, Sent))
        {
            bDead.store(true, std::memory_order_release);
            return false;
        }
        if (Sent <= 0) { bDead.store(true, std::memory_order_release); return false; }
        Offset += Sent;
    }
    return true;
}

bool FSAHttpResponse::WriteSingleBody(int32 StatusCode, const FString& ContentType,
    TConstArrayView<uint8> Body, const TMap<FString, FString>& ExtraHeaders)
{
    if (IsDead()) return false;

    FString Head = FString::Printf(TEXT("HTTP/1.1 %d %s\r\n"), StatusCode, *StatusText(StatusCode));
    Head += FString::Printf(TEXT("Content-Type: %s\r\n"), *ContentType);
    Head += FString::Printf(TEXT("Content-Length: %d\r\n"), Body.Num());
    Head += TEXT("Connection: close\r\n");
    Head += TEXT("Access-Control-Allow-Origin: *\r\n");
    for (const auto& KV : ExtraHeaders)
    {
        Head += FString::Printf(TEXT("%s: %s\r\n"), *KV.Key, *KV.Value);
    }
    Head += TEXT("\r\n");

    if (!WriteAll(Head)) return false;
    return WriteAllRaw(Body);
}

bool FSAHttpResponse::WriteSingleBodyString(int32 StatusCode, const FString& ContentType,
    const FString& Body, const TMap<FString, FString>& ExtraHeaders)
{
    FTCHARToUTF8 Utf8(*Body);
    return WriteSingleBody(StatusCode, ContentType,
        TConstArrayView<uint8>((const uint8*)Utf8.Get(), Utf8.Length()), ExtraHeaders);
}

bool FSAHttpResponse::BeginSSE(const TMap<FString, FString>& ExtraHeaders)
{
    FScopeLock Lock(&SSEWriteLock);
    if (IsDead()) return false;
    FString Head = TEXT("HTTP/1.1 200 OK\r\n")
                   TEXT("Content-Type: text/event-stream\r\n")
                   TEXT("Transfer-Encoding: chunked\r\n")
                   TEXT("Cache-Control: no-cache, no-store, must-revalidate\r\n")
                   TEXT("Connection: keep-alive\r\n")
                   TEXT("Access-Control-Allow-Origin: *\r\n")
                   TEXT("X-Accel-Buffering: no\r\n");
    for (const auto& KV : ExtraHeaders) Head += FString::Printf(TEXT("%s: %s\r\n"), *KV.Key, *KV.Value);
    Head += TEXT("\r\n");
    return WriteAll(Head);
}

// One HTTP/1.1 chunk: hex size CRLF payload CRLF
static FString WrapChunk(const FString& Payload)
{
    FTCHARToUTF8 Utf8(*Payload);
    return FString::Printf(TEXT("%x\r\n%s\r\n"), Utf8.Length(), *Payload);
}

bool FSAHttpResponse::WriteEvent(const FString& EventName, const FString& Data)
{
    FScopeLock Lock(&SSEWriteLock);
    if (IsDead()) return false;
    FString SSE = FString::Printf(TEXT("event: %s\ndata: %s\n\n"), *EventName, *Data);
    return WriteAll(WrapChunk(SSE));
}

bool FSAHttpResponse::WriteKeepAlive()
{
    FScopeLock Lock(&SSEWriteLock);
    if (IsDead()) return false;
    return WriteAll(WrapChunk(TEXT(": keepalive\n\n")));
}

bool FSAHttpResponse::Finish()
{
    FScopeLock Lock(&SSEWriteLock);
    if (IsDead()) return false;
    // Zero-chunk terminator.
    return WriteAll(TEXT("0\r\n\r\n"));
}
```

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAHttpResponse.h \
        Source/SpecialAgent/Private/Transport/SAHttpResponse.cpp
git commit -m "feat(SpecialAgent/transport): add FSAHttpResponse writer (single-body + chunked SSE)"
```

### Task 1.3: Implement FSASessionRegistry (PARALLEL-1c)

**Files:**
- Create: `Source/SpecialAgent/Private/Transport/SASessionRegistry.h`
- Create: `Source/SpecialAgent/Private/Transport/SASessionRegistry.cpp`

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Dom/JsonObject.h"

class FSAConnection;

class FSASessionRegistry
{
public:
    static FSASessionRegistry& Get();

    FString MintSession();

    /** Register the GET /sse connection for a session. If one is already
     *  registered, the previous gets a ": session_replaced\n\n" comment and
     *  is then dropped (last-writer-wins). */
    void RegisterStream(const FString& SessionId, FSAConnection* Conn);
    void UnregisterStream(const FString& SessionId, FSAConnection* Conn);

    bool IsSessionValid(const FString& SessionId) const;

    /** Push a notification to the session's SSE stream. Returns false if the
     *  session has no registered stream or the write failed. */
    bool SendNotification(const FString& SessionId, const TSharedRef<FJsonObject>& Notification);

private:
    FSASessionRegistry() = default;

    mutable FRWLock Lock;
    TSet<FString> ActiveSessions;
    TMap<FString, FSAConnection*> Streams;
};
```

- [ ] **Step 2: Write the implementation**

```cpp
#include "Transport/SASessionRegistry.h"
#include "Transport/SAConnection.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FSASessionRegistry& FSASessionRegistry::Get()
{
    static FSASessionRegistry I;
    return I;
}

FString FSASessionRegistry::MintSession()
{
    const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    FRWScopeLock W(Lock, SLT_Write);
    ActiveSessions.Add(Id);
    return Id;
}

bool FSASessionRegistry::IsSessionValid(const FString& SessionId) const
{
    FRWScopeLock R(Lock, SLT_ReadOnly);
    return ActiveSessions.Contains(SessionId);
}

void FSASessionRegistry::RegisterStream(const FString& SessionId, FSAConnection* Conn)
{
    FSAConnection* Replaced = nullptr;
    {
        FRWScopeLock W(Lock, SLT_Write);
        if (FSAConnection** Existing = Streams.Find(SessionId))
        {
            Replaced = *Existing;
        }
        Streams.Add(SessionId, Conn);
    }
    if (Replaced) Replaced->OnReplacedBySessionRegistry();
}

void FSASessionRegistry::UnregisterStream(const FString& SessionId, FSAConnection* Conn)
{
    FRWScopeLock W(Lock, SLT_Write);
    if (FSAConnection** Existing = Streams.Find(SessionId))
    {
        if (*Existing == Conn) Streams.Remove(SessionId);
    }
}

bool FSASessionRegistry::SendNotification(const FString& SessionId,
                                          const TSharedRef<FJsonObject>& Notification)
{
    FSAConnection* Target = nullptr;
    {
        FRWScopeLock R(Lock, SLT_ReadOnly);
        if (FSAConnection** P = Streams.Find(SessionId)) Target = *P;
    }
    if (!Target) return false;

    FString Payload;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Payload);
    if (!FJsonSerializer::Serialize(Notification, W)) return false;

    return Target->PushSSEEvent(TEXT("message"), Payload);
}
```

> `FSAConnection::OnReplacedBySessionRegistry` and `FSAConnection::PushSSEEvent` are declared in Task 1.4. That's the only coupling point; the registry does not otherwise know about connections.

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SASessionRegistry.h \
        Source/SpecialAgent/Private/Transport/SASessionRegistry.cpp
git commit -m "feat(SpecialAgent/transport): add FSASessionRegistry (session id -> SSE stream map)"
```

### Task 1.4: Implement FSAConnection skeleton (GET/health + POST/mcp single-body response)

**Files:**
- Create: `Source/SpecialAgent/Private/Transport/SAConnection.h`
- Create: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`

Depends on 1.1, 1.2, 1.3. This task defers SSE response mode (Task 2.x) and GET /sse streaming (Task 3b.1).

- [ ] **Step 1: Write the header**

```cpp
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
    void HandleNotFound();

    bool ReadFullRequest(TArray<uint8>& OutBuffer, struct FSAHttpRequest& OutReq);

    FSATcpServer* Owner;
    FSocket* Socket;
    TSharedPtr<FMCPRequestRouter> Router;
    FSAHttpResponse Writer;
    FThreadSafeBool bStopping { false };
    FString ActiveSessionIdForStream;   // set only when acting as GET /sse
};
```

- [ ] **Step 2: Write the implementation (Phase 1 subset — health + mcp single-body)**

```cpp
#include "Transport/SAConnection.h"
#include "Transport/SAHttpParser.h"
#include "Transport/SATransportConstants.h"
#include "Transport/SATcpServer.h"
#include "Transport/SASessionRegistry.h"
#include "MCPRequestRouter.h"
#include "MCPCommon/MCPRequestContext.h"
#include "Async/Async.h"
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
        [Router = Router, Msg = MoveTemp(Msg), P = MoveTemp(P)]() mutable
        {
            FMCPRequestContext Ctx;   // Phase 3 fills this in
            FMCPResponse R = Router->RouteRequest(Msg, Ctx);
            P.SetValue(FSpecialAgentMCPServer::FormatResponse(R));
        });
    const FString Json = Fut.Get();
    Writer.WriteSingleBodyString(200, TEXT("application/json"), Json);
}

uint32 FSAConnection::Run()
{
    TArray<uint8> Buf;
    FSAHttpRequest Req;
    if (!ReadFullRequest(Buf, Req)) return 0;

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
```

> `FSpecialAgentMCPServer::ParseRequest` / `::FormatResponse` need to become **public static** so this new transport can reuse them. Task 1.5 bumps their accessibility.

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAConnection.h \
        Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): add FSAConnection skeleton (health/options/mcp single-body)"
```

### Task 1.5: Expose ParseRequest / FormatResponse + implement FSATcpServer

**Files:**
- Modify: `Source/SpecialAgent/Public/MCPServer.h`
- Modify: `Source/SpecialAgent/Private/MCPServer.cpp`
- Create: `Source/SpecialAgent/Private/Transport/SATcpServer.h`
- Create: `Source/SpecialAgent/Private/Transport/SATcpServer.cpp`

- [ ] **Step 1: Make ParseRequest / FormatResponse accessible**

In `MCPServer.h`, move `ParseRequest` and `FormatResponse` to `public: static` section. (They are already static; just move access.)

- [ ] **Step 2: Write FSATcpServer header**

```cpp
#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

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
```

- [ ] **Step 3: Write FSATcpServer implementation**

```cpp
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
        // 503 then close. Done on a fire-and-forget thread so we don't block accept.
        auto* Reject = new FSAConnection(this, ClientSocket, Router);
        Reject->RequestStop();  // Run() will write the best-effort close; we just leak the thread here intentionally — simpler than juggling threads for a reject path.
        delete Reject;
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
```

- [ ] **Step 4: Update FSAConnection::Run() to call Owner->Retire(this) before returning**

Add `if (Owner) Owner->Retire(this);` at the end of `Run()` (but before `return 0`).

- [ ] **Step 5: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SATcpServer.h \
        Source/SpecialAgent/Private/Transport/SATcpServer.cpp \
        Source/SpecialAgent/Public/MCPServer.h \
        Source/SpecialAgent/Private/MCPServer.cpp \
        Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): add FSATcpServer (FTcpListener + connection registry)"
```

### Task 1.6: Wire FSATcpServer side-by-side with HttpServerModule on port 8768

**Files:**
- Modify: `Source/SpecialAgent/Private/MCPServer.cpp`
- Modify: `Source/SpecialAgent/Public/MCPServer.h`

- [ ] **Step 1: Add FSATcpServer ownership**

In `MCPServer.h`:
```cpp
#include "Transport/SATcpServer.h"
// …
private:
    TUniquePtr<FSATcpServer> RawServer;
```

In `MCPServer.cpp::StartServer`, at the end before logging:
```cpp
RawServer = MakeUnique<FSATcpServer>(RequestRouter);
if (!RawServer->Start(8768))
{
    UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: raw TCP transport failed to bind 8768"));
}
```

In `MCPServer.cpp::StopServer`, before the existing logic:
```cpp
if (RawServer) { RawServer->Stop(); RawServer.Reset(); }
```

- [ ] **Step 2: Commit**

```bash
git add Source/SpecialAgent/Public/MCPServer.h Source/SpecialAgent/Private/MCPServer.cpp
git commit -m "feat(SpecialAgent/transport): mount raw TCP transport on :8768 side-by-side"
```

- [ ] **Step 3: Build and smoke-test (manual gate)**

Rebuild plugin → editor loads → `curl http://localhost:8768/health` returns `{"status":"healthy",...}`. `curl -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":"1","method":"tools/list"}' http://localhost:8768/mcp` returns 300 tools.

### Task 1.7: Add IMCPService::HandleRequest signature bump

**Files:**
- Modify: `Source/SpecialAgent/Public/Services/IMCPService.h`
- Modify: `Source/SpecialAgent/Private/MCPRequestRouter.h`
- Modify: `Source/SpecialAgent/Private/MCPRequestRouter.cpp`

- [ ] **Step 1: Bump the interface**

```cpp
// IMCPService.h
#include "MCPCommon/MCPRequestContext.h"
// …
class IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request,
                                       const FString& MethodName,
                                       const FMCPRequestContext& Ctx) = 0;
    // …
};
```

- [ ] **Step 2: Bump the router**

`FMCPRequestRouter::RouteRequest` gains `const FMCPRequestContext&`. Its one call to `Service->HandleRequest(ModifiedRequest, MethodName)` at line ~432 becomes `Service->HandleRequest(ModifiedRequest, MethodName, Ctx)`.

- [ ] **Step 3: Commit (will break build until Task 1.8 lands)**

```bash
git add Source/SpecialAgent/Public/Services/IMCPService.h \
        Source/SpecialAgent/Private/MCPRequestRouter.h \
        Source/SpecialAgent/Private/MCPRequestRouter.cpp
git commit -m "feat(SpecialAgent): bump IMCPService::HandleRequest to take FMCPRequestContext (interface only)"
```

### Task 1.8: Mechanical signature bump across all 45 services (PARALLEL FAN-OUT)

**Files:**
- Modify: `Source/SpecialAgent/Public/Services/*.h` — 45 headers
- Modify: `Source/SpecialAgent/Private/Services/*.cpp` — 45 implementations

Every `HandleRequest(const FMCPRequest& Request, const FString& MethodName)` becomes `HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)`. No handler body changes needed in Phase 1 — `Ctx` is just ignored.

- [ ] **Step 1: Dispatch parallel subagents**

Split the 45 services into 5 groups of 9. Dispatch 5 Explore-type subagents in parallel, each with this prompt:

> Find these N services' `HandleRequest` declarations (in `Source/SpecialAgent/Public/Services/XXXService.h`) and implementations (in `Source/SpecialAgent/Private/Services/XXXService.cpp`) and add a 3rd parameter `const FMCPRequestContext& Ctx` to both. Include `"MCPCommon/MCPRequestContext.h"` in each `.cpp`. Do NOT modify any method body. After editing, run `git diff --stat` and report.

- [ ] **Step 2: Verify uniform change**

```bash
git diff --stat | wc -l       # expect 90 files (45 .h + 45 .cpp)
git grep -n 'HandleRequest(const FMCPRequest& Request, const FString& MethodName)' Source/SpecialAgent
# expect: no hits (all bumped)
```

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent
git commit -m "refactor(SpecialAgent/services): mechanical HandleRequest signature bump (+Ctx) across 45 services"
```

### Task 1.9: Cut over to raw TCP on 8767, delete HttpServerModule usage

**Files:**
- Modify: `Source/SpecialAgent/Public/MCPServer.h`
- Modify: `Source/SpecialAgent/Private/MCPServer.cpp`

- [ ] **Step 1: Delete HttpServerModule routes**

Remove from `StartServer`:
- `FHttpServerModule::Get().StartAllListeners()`
- `HttpRouter->BindRoute(...)` × 6
- `HttpServerModule` includes

Remove from `StopServer`:
- `HttpRouter->UnbindRoute(...)` × 3

- [ ] **Step 2: Point FSATcpServer at 8767**

`RawServer->Start(ServerPort)` (was `8768`).

- [ ] **Step 3: Delete dead methods**

`HandleMessage`, `HandleSSEConnection`, `HandleCORS`, `HandleHealth` — all gone. `SSEConnections` map, `ConnectionsLock`, `SSERouteHandle`, `MessageRouteHandle`, `HealthRouteHandle` — gone. `SendSSEEvent`, `BroadcastSSEEvent`, `CleanupConnections`, `GenerateSessionId` — gone.

- [ ] **Step 4: Build and run full smoke matrix**

Run `docs/superpowers/specs/sse-transport-smoke.sh` (scaffolded in Task 1.10 below; for now run these curls inline):
```bash
curl -s http://localhost:8767/health | jq .status                        # "healthy"
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":"1","method":"tools/list"}' \
    http://localhost:8767/mcp | jq '.result.tools | length'              # 300
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":"2","method":"tools/call","params":{"name":"pie/is_playing","arguments":{}}}' \
    http://localhost:8767/mcp | jq .result
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":"3","method":"ping"}' \
    http://localhost:8767/mcp | jq .result
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":"4","method":"resources/templates/list"}' \
    http://localhost:8767/mcp | jq .result
curl -s -I -X OPTIONS http://localhost:8767/mcp | head -1                # HTTP/1.1 204
```
All must succeed.

- [ ] **Step 5: Commit**

```bash
git add Source/SpecialAgent/Public/MCPServer.h Source/SpecialAgent/Private/MCPServer.cpp
git commit -m "refactor(SpecialAgent): cut MCPServer over to raw TCP transport on :8767; delete HttpServerModule routes"
```

### Task 1.10: Create the smoke script

**Files:**
- Create: `docs/superpowers/specs/sse-transport-smoke.sh`

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# sse-transport-smoke.sh — integration smoke for the SpecialAgent raw-TCP MCP transport.
# Requires jq. Intended to be run against a live editor on 127.0.0.1:8767.

set -euo pipefail
HOST="${HOST:-http://127.0.0.1:8767}"
FAIL=0
pass() { echo "ok  $1"; }
fail() { echo "FAIL $1: $2"; FAIL=1; }

# 1. /health
R=$(curl -sS "$HOST/health") || { fail health "no response"; exit 1; }
[ "$(echo "$R" | jq -r .status)" = "healthy" ] && pass health || fail health "status not healthy: $R"

# 2. initialize → session id
R=$(curl -sS -D - -o /tmp/init.json -X POST -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' "$HOST/mcp")
SID=$(echo "$R" | tr -d '\r' | awk -F': ' 'BEGIN{IGNORECASE=1} /^Mcp-Session-Id/ {print $2; exit}')
[ -n "$SID" ] && pass initialize.session_header || fail initialize.session_header "no Mcp-Session-Id header"

# 3. tools/list carrying session
N=$(curl -sS -X POST -H 'Content-Type: application/json' -H "Mcp-Session-Id: $SID" \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' "$HOST/mcp" | jq '.result.tools | length')
[ "$N" = "300" ] && pass tools.list.count=300 || fail tools.list.count "got $N"

# 4. tools/call level/get_current_path
STATUS=$(curl -sS -X POST -H 'Content-Type: application/json' -H "Mcp-Session-Id: $SID" \
    -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"level/get_current_path","arguments":{}}}' "$HOST/mcp" \
    | jq -r '.result.isError // false')
[ "$STATUS" = "false" ] && pass level.get_current_path || fail level.get_current_path "isError=$STATUS"

# 5. missing session id on non-initialize
CODE=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":4,"method":"tools/list"}' "$HOST/mcp")
[ "$CODE" = "400" ] && pass missing.session_id=400 || fail missing.session_id "got $CODE"

# 6. OPTIONS preflight
CODE=$(curl -sS -o /dev/null -w '%{http_code}' -X OPTIONS "$HOST/mcp")
[ "$CODE" = "204" ] && pass options.204 || fail options.204 "got $CODE"

# 7. /sse without session
CODE=$(curl -sS -o /dev/null -w '%{http_code}' "$HOST/sse")
[ "$CODE" = "400" ] && pass sse.no_session=400 || fail sse.no_session "got $CODE"

exit $FAIL
```

> In Phase 1, cases 5 and 7 are expected to fail (session enforcement lands in Phase 3a). Mark them as TODO and temporarily expect `200` / `501`. Update at Task 3a.2.

- [ ] **Step 2: Make executable**

```bash
chmod +x docs/superpowers/specs/sse-transport-smoke.sh
```

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/sse-transport-smoke.sh
git commit -m "test(SpecialAgent): add integration smoke script for MCP transport"
```

### Task 1.11: Phase 1 gate — full smoke + 300-tool round-trip

- [ ] **Step 1: Rebuild plugin, restart editor**

- [ ] **Step 2: Run smoke**

```bash
docs/superpowers/specs/sse-transport-smoke.sh
```

Expected: all phase-1 cases (1, 2 partial, 3, 4, 6) pass. 5 and 7 marked TODO.

- [ ] **Step 3: Run parser automation tests**

```
UnrealEditor -ExecCmds="Automation RunTests SpecialAgent.Transport.Parser; Quit"
```

Expected: all 5 parser tests pass.

- [ ] **Step 4: Tag the milestone**

```bash
git tag sse-transport-phase-1
```

---

## Phase 2 — POST SSE response + keep-alive

### Task 2.1: Detect Accept header, branch response mode

**Files:**
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`

- [ ] **Step 1: In `HandlePostMCP`, check Accept header early**

```cpp
const bool bSSEResponse = Req.AcceptContains(TEXT("text/event-stream"));
```

Split the two paths. Single-body path is existing code. SSE path is implemented in Task 2.2.

- [ ] **Step 2: Commit (branch only, SSE path still goes to single-body until 2.2)**

```bash
git add Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): detect Accept: text/event-stream in POST handler"
```

### Task 2.2: Implement SSE response + keep-alive ticker

**Files:**
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`

- [ ] **Step 1: Implement the SSE branch**

```cpp
void FSAConnection::HandlePostMCPSSE(const FSAHttpRequest& Req)
{
    // Parse JSON-RPC up front so we can 200 + headers immediately.
    const FString BodyStr = FString::ConstructFromPtrSize(
        UTF8_TO_TCHAR((const ANSICHAR*)Req.Body.GetData()), Req.Body.Num());
    FMCPRequest Msg;
    FSpecialAgentMCPServer::ParseRequest(BodyStr, Msg);   // parse errors reported as event below

    if (!Writer.BeginSSE()) return;

    // Dispatch on background thread.
    std::atomic<bool> bDone{false};
    TPromise<FString> P;
    auto Fut = P.GetFuture();
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [Router = Router, Msg, &bDone, P = MoveTemp(P)]() mutable
        {
            FMCPRequestContext Ctx;       // Phase 3 fills this in
            FMCPResponse R = Router->RouteRequest(Msg, Ctx);
            P.SetValue(FSpecialAgentMCPServer::FormatResponse(R));
            bDone.store(true, std::memory_order_release);
        });

    // Keep-alive ticker: flush every kKeepAliveIntervalSeconds until handler finishes.
    while (!bDone.load(std::memory_order_acquire) && !bStopping)
    {
        for (int32 i = 0;
             i < SATransport::KeepAliveIntervalSeconds * 10
             && !bDone.load(std::memory_order_acquire)
             && !bStopping;
             ++i)
        {
            FPlatformProcess::Sleep(0.1f);
        }
        if (!bDone.load(std::memory_order_acquire) && !bStopping)
        {
            if (!Writer.WriteKeepAlive()) break;
        }
    }

    if (!Writer.IsDead())
    {
        const FString Json = Fut.Get();
        Writer.WriteEvent(TEXT("message"), Json);
        Writer.Finish();
    }
}
```

- [ ] **Step 2: Wire the branch in `HandlePostMCP`**

```cpp
if (Req.AcceptContains(TEXT("text/event-stream")))
{
    HandlePostMCPSSE(Req);
}
else
{
    // existing single-body path
}
```

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): SSE response mode for POST with keep-alive ticker"
```

### Task 2.3: Phase 2 gate — material normals test

- [ ] **Step 1: Rebuild plugin, restart editor**

- [ ] **Step 2: Smoke-test SSE response shape**

```bash
curl -Ns -X POST \
     -H 'Content-Type: application/json' \
     -H 'Accept: text/event-stream' \
     -d '{"jsonrpc":"2.0","id":"1","method":"tools/call","params":{"name":"pie/is_playing","arguments":{}}}' \
     http://127.0.0.1:8767/mcp
```

Expected: chunked `text/event-stream` response terminating with `event: message\ndata: {...}\n\n`.

- [ ] **Step 3: Dogfood via Claude Code**

Attach Claude Code. Invoke a material operation that triggers normal-map computation (e.g. via `python/execute` + `unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([...])` on an FBX with `bGenerateNormalMaps=True`). Confirm success; previously timed out.

- [ ] **Step 4: Tag**

```bash
git tag sse-transport-phase-2
```

---

## Phase 3a — Session machinery + Context wiring

### Task 3a.1: MCPRequestContext construction in FSAConnection

**Files:**
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`

- [ ] **Step 1: Build real FMCPRequestContext before dispatch**

Replace the empty-`Ctx` line in `HandlePostMCP` / `HandlePostMCPSSE` with:

```cpp
FMCPRequestContext Ctx;
Ctx.SessionId = Req.GetHeader(TEXT("Mcp-Session-Id"));
// Extract progressToken from params._meta.progressToken (if present)
if (Msg.Params.IsValid())
{
    const TSharedPtr<FJsonObject>* Meta = nullptr;
    if (Msg.Params->TryGetObjectField(TEXT("_meta"), Meta))
    {
        Ctx.ProgressToken = (*Meta)->TryGetField(TEXT("progressToken"));
    }
}
// SendProgress bound in Task 3b.4.
Ctx.SendProgress = [](double, double, const FString&){};  // no-op placeholder
```

- [ ] **Step 2: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): populate FMCPRequestContext from request"
```

### Task 3a.2: Mint session id in initialize; validate on other POSTs

**Files:**
- Modify: `Source/SpecialAgent/Private/MCPRequestRouter.cpp`
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`
- Modify: `docs/superpowers/specs/sse-transport-smoke.sh`

- [ ] **Step 1: Mint session in initialize**

In `FMCPRequestRouter::HandleInitialize`, at the end, register the minted id on the response. The response struct doesn't carry headers today — pass it back via `FMCPRequestContext` being mutable, or add an `FString OutSessionId` on `FMCPResponse`. Pick the latter — smaller change.

- [ ] **Step 2: Thread session id from response to HTTP header in SAConnection**

When `Msg.Method == "initialize"`, mint via `FSASessionRegistry::Get().MintSession()` and add the `Mcp-Session-Id` response header. (Simpler and keeps the router ignorant of transport.)

- [ ] **Step 3: Validate on non-initialize POSTs**

In `HandlePostMCP`, before dispatch:

```cpp
if (Msg.Method != TEXT("initialize"))
{
    const FString Sid = Req.GetHeader(TEXT("Mcp-Session-Id"));
    if (Sid.IsEmpty())
    {
        Writer.WriteSingleBodyString(400, TEXT("application/json"),
            TEXT("{\"error\":\"missing Mcp-Session-Id\"}"));
        return;
    }
    if (!FSASessionRegistry::Get().IsSessionValid(Sid))
    {
        Writer.WriteSingleBodyString(404, TEXT("application/json"),
            TEXT("{\"error\":\"unknown session; re-initialize\"}"));
        return;
    }
}
```

- [ ] **Step 4: Update smoke script**

Cases 5 and 7 now enforce and should pass.

- [ ] **Step 5: Commit**

```bash
git add Source/SpecialAgent
git add docs/superpowers/specs/sse-transport-smoke.sh
git commit -m "feat(SpecialAgent/transport): mint + validate Mcp-Session-Id across POST /mcp"
```

---

## Phase 3b — Long-lived GET /sse + progress notifications

### Task 3b.1: Implement GET /sse handler

**Files:**
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.h`

- [ ] **Step 1: Add HandleGetSSE**

```cpp
void FSAConnection::HandleGetSSE(const FSAHttpRequest& Req)
{
    const FString Sid = Req.GetHeader(TEXT("Mcp-Session-Id"));
    if (Sid.IsEmpty())
    {
        Writer.WriteSingleBodyString(400, TEXT("application/json"),
            TEXT("{\"error\":\"missing Mcp-Session-Id\"}"));
        return;
    }
    if (!FSASessionRegistry::Get().IsSessionValid(Sid))
    {
        Writer.WriteSingleBodyString(404, TEXT("application/json"),
            TEXT("{\"error\":\"unknown session\"}"));
        return;
    }
    ActiveSessionIdForStream = Sid;
    if (!Writer.BeginSSE()) return;
    FSASessionRegistry::Get().RegisterStream(Sid, this);

    // Keep-alive loop. Exits on bStopping or socket death.
    while (!bStopping && !Writer.IsDead())
    {
        for (int32 i = 0;
             i < SATransport::KeepAliveIntervalSeconds * 10 && !bStopping && !Writer.IsDead();
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
    Writer.Finish();
    ActiveSessionIdForStream.Empty();
}
```

- [ ] **Step 2: Replace the 501 stub in Run()**

```cpp
else if (Req.Verb == TEXT("GET") && Req.Path == TEXT("/sse")) HandleGetSSE(Req);
```

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAConnection.h \
        Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): GET /sse long-lived stream with keep-alive"
```

### Task 3b.2: Bind real SendProgress closure

**Files:**
- Modify: `Source/SpecialAgent/Private/Transport/SAConnection.cpp`

- [ ] **Step 1: Replace the placeholder SendProgress**

```cpp
const FString SessionIdCopy = Ctx.SessionId;
const TSharedPtr<FJsonValue> TokenCopy = Ctx.ProgressToken;
Ctx.SendProgress = [SessionIdCopy, TokenCopy](double Progress, double Total, const FString& Message)
{
    if (SessionIdCopy.IsEmpty()) return;  // no session, nowhere to send
    TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
    N->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    N->SetStringField(TEXT("method"), TEXT("notifications/progress"));
    TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    if (TokenCopy.IsValid())       Params->SetField(TEXT("progressToken"), TokenCopy);
    Params->SetNumberField(TEXT("progress"), Progress);
    Params->SetNumberField(TEXT("total"),    Total);
    if (!Message.IsEmpty())        Params->SetStringField(TEXT("message"), Message);
    N->SetObjectField(TEXT("params"), Params);
    FSASessionRegistry::Get().SendNotification(SessionIdCopy, N);
};
```

- [ ] **Step 2: Commit**

```bash
git add Source/SpecialAgent/Private/Transport/SAConnection.cpp
git commit -m "feat(SpecialAgent/transport): bind FMCPRequestContext::SendProgress to session SSE stream"
```

### Task 3b.3: Smoke-test progress emission

**Files:**
- Temp-modify: pick one service (suggest `FPythonService::HandleExecute`) to emit `Ctx.SendProgress(0.5, 1.0, TEXT("half"))` once.

- [ ] **Step 1: Add a single SendProgress call in the chosen handler (TEMPORARY)**

- [ ] **Step 2: Run the GET /sse smoke**

```bash
# Shell 1: open stream
curl -N -H "Mcp-Session-Id: <SID>" http://localhost:8767/sse &
# Shell 2: call the tool
curl -s -X POST -H 'Content-Type: application/json' -H "Mcp-Session-Id: <SID>" \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"python/execute","arguments":{"code":"print(1)"}}}' \
     http://localhost:8767/mcp
```

Expected: Shell 1 receives `event: message\ndata: {"jsonrpc":"2.0","method":"notifications/progress",...}\n\n` before the POST completes.

- [ ] **Step 3: Revert the temporary SendProgress (unless you want to keep it), tag**

```bash
git revert <SHA of step 1>
git tag sse-transport-phase-3
```

---

## Phase 4 — Cleanup

### Task 4.1: Remove HTTPServer module dependency

**Files:**
- Modify: `Source/SpecialAgent/SpecialAgent.Build.cs`

- [ ] **Step 1: Drop `"HTTPServer"` from `PublicDependencyModuleNames`**

- [ ] **Step 2: Confirm build still succeeds**

- [ ] **Step 3: Commit**

```bash
git add Source/SpecialAgent/SpecialAgent.Build.cs
git commit -m "chore(SpecialAgent): drop HTTPServer module dependency"
```

### Task 4.2: Grep-verify no stale references

- [ ] **Step 1: Search**

```bash
git grep -nE 'FHttpServerModule|FHttpServerResponse|FHttpRouter|FHttpRequestHandler|FHttpResultCallback|IHttpRouter' Source/SpecialAgent
```

Expected: no hits.

- [ ] **Step 2: Tag**

```bash
git tag sse-transport-phase-4
```

### Task 4.3: Update MCPServer.cpp header, remove dead forward declarations

**Files:**
- Modify: `Source/SpecialAgent/Public/MCPServer.h`

- [ ] **Step 1: Drop `SSEConnections`, `FSSEConnection`, `ConnectionsLock`, `SSERouteHandle`, `MessageRouteHandle`, `HealthRouteHandle`, `HttpRouter` and any includes not needed.**

- [ ] **Step 2: Commit**

```bash
git add Source/SpecialAgent/Public/MCPServer.h
git commit -m "chore(SpecialAgent): remove dead SSE placeholder state from MCPServer.h"
```

### Task 4.4: Final smoke, Claude Code dogfood, merge

- [ ] **Step 1: Run full smoke script, parser automation, 300-tool round-trip.**

- [ ] **Step 2: Dogfood via Claude Code — run all three operation categories (fast / medium / long).**

- [ ] **Step 3: Merge `feature/sse-transport` into `main` and push to `fork`.**

```bash
# from plugin root (not worktree)
git checkout main
git merge --no-ff feature/sse-transport
git push fork main
```

- [ ] **Step 4: Invoke superpowers:finishing-a-development-branch to clean up the worktree.**

---

## Parallel-execution map

For the subagent-driven execution skill, these tasks can run in parallel without shared state:

| Group | Tasks | Why parallelizable |
|---|---|---|
| A (Phase 1 scaffolding) | 1.1 (parser), 1.2 (response), 1.3 (session registry) | Three different new files, no mutual references. |
| B (Signature bump) | 1.8 (5 fan-out agents, 9 services each) | Each agent touches a disjoint set of files. |

Tasks 1.4 and onward must be serial — each depends on the output of the previous.

---

## Risks & recovery

| Risk | Recovery |
|---|---|
| Parser test fails on Windows line endings | Add CRLF-vs-LF robustness; parser already CRLF-only per spec. Re-run. |
| FTcpListener bind fails (port held by stale editor) | `lsof -i :8767` + kill; or restart editor. |
| Signature-bump agent misses a file | `git grep 'HandleRequest(const FMCPRequest& Request, const FString& MethodName)' Source/SpecialAgent` catches it; fix and re-commit. |
| Keep-alive never flushes (clock drift / sleep granularity) | Switch from `FPlatformProcess::Sleep` loop to `FEvent::Wait` with timeout. |
| Claude Code reports `Mcp-Session-Id` not honored | Check the response-header path in Task 3a.2; add response-header support to `FMCPResponse` struct properly. |
| Port 8767 bind race at editor startup | Retry `FSATcpServer::Start` a few times with exponential backoff. |

---

## Acceptance summary

All four phases gated by:
- Clean build on all phases' commits.
- Parser automation suite passes.
- Smoke script passes end-to-end.
- Claude Code dogfood completes fast / medium / long tool calls without timeout.
- Grep check post-Phase-4 shows zero HttpServerModule references.
- All 300 tools still callable.

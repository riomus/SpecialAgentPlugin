# SSE Transport for SpecialAgent MCP Server — Design

**Status:** Approved
**Date:** 2026-04-20
**Branch:** `feature/sse-transport` (worktree at `.worktrees/sse-transport/`)
**Supersedes:** The SSE placeholder in `MCPServer.cpp::HandleSSEConnection` (which closes the stream after one event).

## Problem

Long-running tool calls — in particular `material/*` operations that trigger normal-map or ORM texture computation — fail when invoked from Claude Code. The client's HTTP POST read times out before the handler finishes, even though the handler is still making progress on the editor side.

The current transport uses UE's `FHttpServerModule`, whose public API is one-shot: the handler builds a complete `FHttpServerResponse` and calls `OnComplete(...)` exactly once, which closes the connection. There is no chunked encoding, no partial writes, no way to keep the client's connection alive during a long handler. MCP's Streamable HTTP transport requires the server to be able to respond with `text/event-stream` and stream events back over time; UE's HTTPServer module cannot produce that shape at all.

Two fixes are required:

1. **Replace the HTTP transport layer** with one capable of real streaming (chunked transfer encoding / SSE).
2. **Keep the client's POST alive** during long handlers by flushing SSE frames — either keep-alive comments or `notifications/progress` events — so the client's transport does not give up.

## Non-goals

- HTTP/2 or TLS support — editor runs locally; clients connect over `http://127.0.0.1`.
- Arbitrary request-body streaming — handlers all consume small JSON-RPC payloads.
- Request pipelining — MCP clients don't use it.
- Automatic session expiry / eviction — editor restart clears all sessions; memory pressure from leaked sessions is negligible (a small struct per `initialize`).
- Progress cancellation — handlers run to completion; cancellation would require engine-side interruption support that most handlers (asset import, material compile) don't provide.

## Key design decisions

The decisions below were locked in during brainstorming:

| # | Decision | Alternatives considered | Why |
|---|---|---|---|
| 1 | Scope = all four phases: raw TCP, POST SSE, persistent GET /sse, cleanup | Phase 2 only (minimum viable fix) | User wants full SSE done in one go, including progress notifications. |
| 2 | Endpoints: `POST /mcp`, `GET /sse`, `GET /health`. Drop `/message`. Drop GET on `/mcp`. | Keep all five current paths | `/message` was legacy; the modern MCP Streamable HTTP transport uses a single URL for POST, a separate URL for the server-push SSE stream. |
| 3 | Thread-per-connection concurrency | Thread pool; `AsyncTask` dispatch | Real-world concurrency is 1–3 clients. Thread per connection is the simplest correct design. Hard cap: 16 concurrent connections. |
| 4 | Progress plumbed through `IMCPService::HandleRequest` via an added `const FMCPRequestContext& Ctx` parameter | Thread-local notifier; deferring progress | Explicit and compiler-enforced. Mechanical 45-file signature bump. |
| 5 | Full session machinery: server mints `Mcp-Session-Id` during `initialize`, validates on every subsequent POST, keys notification routing by it | Single-session simplification; no sessions | Spec-correct; small incremental cost given we're already rewriting transport. |
| 6 | Hard cutover: delete `FHttpServerModule` code once new transport is wired | CVar feature flag; fallback-on-error | Single-developer editor tool, small blast radius, cleaner history. |

## Architecture

The new transport layer **replaces `FHttpServerModule` entirely**. The surface above the socket is unchanged: `FMCPRequestRouter`, all 45 services, `FGameThreadDispatcher`, and `FMCPGameThreadProcessor` continue to work exactly as they do today.

```
┌──────────────────────────────────────────────────────────────────┐
│ Claude Code / Cursor / other MCP client                          │
└───────────────┬──────────────────────────────────┬───────────────┘
                │ POST /mcp                        │ GET /sse (long-lived)
┌───────────────▼──────────────────────────────────▼───────────────┐
│ FSATcpServer  (FTcpListener, accept loop, active-conn registry)  │
│   └─ FSAConnection × N  (one FRunnable worker thread per socket) │
│        ├─ FSAHttpParser  (request-line + headers + body)         │
│        ├─ FSAHttpResponse (single-body OR chunked SSE writer)    │
│        └─ uses FSASessionRegistry                                │
└───────────────┬──────────────────────────────────────────────────┘
                │ FMCPRequestContext  {SessionId, ProgressToken, SendProgress}
┌───────────────▼──────────────────────────────────────────────────┐
│ FMCPRequestRouter  → IMCPService::HandleRequest(Req, Method, Ctx)│
│   ├─ FPIEService, FWorldService, … (45 services — unchanged above│
│   │                                   interface-bump)            │
│   └─ FGameThreadDispatcher  → FMCPGameThreadProcessor (tick)     │
└──────────────────────────────────────────────────────────────────┘
```

### Module layout (new, in worktree)

```
Source/SpecialAgent/Private/Transport/
    SATcpServer.h / .cpp          — FTcpListener, accept loop, connection registry
    SAConnection.h / .cpp         — per-connection FRunnable; HTTP parse; dispatch; SSE writer
    SAHttpParser.h / .cpp         — request-line + headers + fixed-length body parser
    SAHttpResponse.h / .cpp       — response writer (single-body OR chunked SSE)
    SASessionRegistry.h / .cpp    — session id → active SSE stream lookup, thread-safe
    SANotifier.h                  — interface used by FMCPRequestContext to push progress
Source/SpecialAgent/Public/MCPCommon/
    MCPRequestContext.h           — session id, progress token, SendProgress callback
```

### Existing files modified

- `MCPServer.cpp/.h` — `FSpecialAgentMCPServer` becomes a thin wrapper: owns the `FSATcpServer`, exposes `Start` / `Stop` / `RecordClientActivity` / `GetConnectedClientCount`. All `HttpRouter` / `FHttpServerResponse` / `HandleMessage` plumbing deleted.
- `IMCPService.h` — `HandleRequest` signature grows a `const FMCPRequestContext& Ctx` parameter. All 45 service `.cpp` files receive the mechanical bump.
- `MCPRequestRouter.cpp/.h` — `RouteRequest` takes `FMCPRequestContext` and forwards it to the service.
- `SpecialAgent.Build.cs` — drop `"HTTPServer"` from public deps; keep `"Sockets"` and `"Networking"` (already present).

### Separation of concerns

- `FSATcpServer` owns the listener and the set of active connections. Knows nothing about HTTP, SSE, or MCP.
- `FSAConnection` owns one socket's lifetime: parse → dispatch → write. Knows HTTP but not MCP protocol details.
- `FSAHttpParser` / `FSAHttpResponse` know HTTP/1.1 framing but nothing above.
- `FMCPRequestRouter` knows MCP but nothing about transport.
- `FSASessionRegistry` bridges: maps `Mcp-Session-Id` → open SSE connection so notifications can find their destination.

Every unit can be understood and tested in isolation.

## Component contracts

### `FSATcpServer`

**Purpose:** listen on port 8767, accept connections, own their lifetimes.

**API:**
```cpp
class FSATcpServer {
public:
    bool Start(int32 Port);
    void Stop();
    int32 GetActiveConnectionCount() const;
};
```

**Internals:** `FTcpListener` with an `OnConnectionAccepted` delegate. For each accepted `FSocket*`, construct an `FSAConnection` and push it onto the active-connection list (guarded by `FCriticalSection`). On `Stop()`, signal all connections to close and join their threads (bounded by the 15 s keep-alive cadence worst case).

**Dependencies:** `Sockets`, `Networking`, `FSAConnection`.

### `FSAConnection` (one per socket, `FRunnable`)

**Purpose:** drive one socket from accept through close.

**Lifecycle:**
1. Parse an HTTP request via `FSAHttpParser`.
2. Route by verb + path:
   - `POST /mcp` → deserialize JSON-RPC → build `FMCPRequestContext` → dispatch to `FMCPRequestRouter::RouteRequest` on a background task → write response as `application/json` **or** `text/event-stream` based on the client's `Accept` header.
   - `GET /sse` → validate `Mcp-Session-Id` → register self with `FSASessionRegistry` → hold open and stream keep-alives + server-pushed notifications until client closes.
   - `GET /health` → one-shot JSON.
   - `OPTIONS *` → CORS preflight.
3. Close socket, remove from registry / server list.

**Concurrency:** exactly one thread per connection. No shared mutable state across connections except the session registry.

**Dependencies:** `FSAHttpParser`, `FSAHttpResponse`, `FSASessionRegistry`, `FMCPRequestRouter`, `FGameThreadDispatcher` (indirect).

### `FSAHttpParser`

**Purpose:** parse an HTTP/1.1 request from a socket.

**Supported:** `GET`, `POST`, `OPTIONS`; request-line + headers terminated by `\r\n\r\n`; fixed-length body via `Content-Length`.

**Explicitly unsupported (v1):** chunked *request* bodies, pipelining, HTTP/2. Malformed requests produce well-formed 4xx responses.

**Limits:** headers ≤ 16 KiB, body ≤ 16 MiB.

**Style:** given bytes in, produce a `FSAHttpRequest` struct or an error — pure, no I/O.

### `FSAHttpResponse`

**Purpose:** serialize a response back to the socket. Two modes:

- **Single-body mode:** status line + headers + `Content-Length` + body (for `/health`, JSON responses, OPTIONS).
- **Chunked-SSE mode:** status line + `Content-Type: text/event-stream` + `Transfer-Encoding: chunked` headers; then `WriteEvent(FString Event, FString Data)` writes one SSE frame as one HTTP chunk; `WriteKeepAlive()` writes `: keepalive\n\n`; `Finish()` writes the zero-chunk terminator.

**Thread safety:** exclusive to its owning `FSAConnection`, but the SSE writer needs a `FCriticalSection` (`SSEWriteLock`) because notifications can arrive from the router's background task while the connection thread is also writing keep-alives.

### `FSASessionRegistry`

**Purpose:** map session id → the open `GET /sse` connection for that session.

**API:**
```cpp
class FSASessionRegistry {
public:
    FString MintSession();
    void Register(const FString& SessionId, FSAConnection* Conn);
    void Unregister(const FString& SessionId);
    bool SendNotification(const FString& SessionId, TSharedPtr<FJsonObject> Notification);
    bool IsSessionValid(const FString& SessionId) const;
};
```

**Thread safety:** `FRWLock` — readers (notification senders) can run in parallel; writers (register / unregister) serialize.

### `FMCPRequestContext`

**Purpose:** request-scoped context passed into every `IMCPService::HandleRequest`.

**Definition:**
```cpp
struct FMCPRequestContext {
    FString SessionId;
    TSharedPtr<FJsonValue> ProgressToken;   // from params._meta.progressToken, may be null
    TFunction<void(double Progress, double Total, const FString& Message)> SendProgress;
    // SendProgress is always safe to call; if no session or no SSE stream, it's a no-op.
};
```

**Construction:** built by `FSAConnection` before calling `FMCPRequestRouter::RouteRequest`. `SendProgress` closure looks up the session's SSE connection via `FSASessionRegistry` and writes a `notifications/progress` event. If the session has no SSE stream, the call is silently dropped.

### `IMCPService::HandleRequest` — signature change

```cpp
// Before:
virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) = 0;
// After:
virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) = 0;
```

Mechanical bump across 45 `.cpp` + corresponding `.h` service files. Most implementations ignore `Ctx`. Services that want progress call `Ctx.SendProgress(0.5, 1.0, TEXT("Halfway"))`.

## Data flow

Three representative flows. Everything MCP-specific sits above the `FSAConnection` boundary.

### Flow A — Fast POST `/mcp` (tool call, < 1 s)

```
Client                   FSAConnection        Background task     Game thread
  │                          │                      │                  │
  │── POST /mcp (Accept: application/json) ──▶      │                  │
  │                   parse HTTP                    │                  │
  │                   decide: JSON response         │                  │
  │                          │──── AsyncTask ──────▶│                  │
  │                          │               RouteRequest()            │
  │                          │               Service->HandleRequest(Ctx)
  │                          │               DispatchToGameThreadSync ─▶
  │                          │                      │      run on tick │
  │                          │                      │◀──────────────── │
  │                          │◀── response JSON ────│                  │
  │                   format 200 OK + body          │                  │
  │◀── response ─────────────│                      │                  │
  │                   close connection              │                  │
```

Identical to today. No SSE involved.

### Flow B — Long POST `/mcp` (material normal compute)

```
Client                   FSAConnection           Background task     Writer mutex
  │                          │                         │                 │
  │── POST /mcp (Accept: text/event-stream) ──▶        │                 │
  │                   parse HTTP; Accept includes SSE  │                 │
  │                   write: 200 OK,                    │                 │
  │                          Content-Type: text/event-stream,            │
  │                          Transfer-Encoding: chunked                  │
  │◀── headers only ─────────│                         │                 │
  │                          │──── AsyncTask ─────────▶│                 │
  │                   start keep-alive timer (15 s)    │                 │
  │                          │               RouteRequest() working...   │
  │◀── ": keepalive\n\n" ────│─────────────────── acquire ────────────▶ │
  │◀── ": keepalive\n\n" ────│                         │                 │
  │◀── ": keepalive\n\n" ────│                         │                 │
  │                          │                         │ Ctx.SendProgress(0.5)
  │                          │                         │── acquire ────▶│
  │◀── event:message/progress│                         │                 │
  │                          │               …finishes, returns response │
  │                          │                         │── acquire ────▶│
  │◀── event: message / data: <resp> ─│                │                 │
  │                   stop keep-alive timer            │                 │
  │                   write zero-chunk terminator      │                 │
  │                   close connection                 │                 │
```

Headers flush immediately → client's transport timeout clock never starts counting from a silent socket. Keep-alives and progress notifications serialize through `SSEWriteLock` to avoid interleaving.

### Flow C — Long-lived `GET /sse`

```
Client              FSAConnection     FSASessionRegistry     Service handler
  │                      │                   │                     │
  │── GET /sse (Mcp-Session-Id: X) ──▶       │                     │
  │               validate session X         │                     │
  │                      │── Register(X, self) ▶                   │
  │               write 200 OK text/event-stream chunked headers   │
  │◀── headers ──────────│                   │                     │
  │               enter keep-alive loop; block on stop-signal      │
  │                      │                   │                     │
  │          (a POST /mcp call runs)         │                     │
  │                      │                   │ Ctx.SendProgress() ─│
  │                      │                   │◀────────────────────│
  │                      │                   │ lookup X → conn     │
  │                      │◀── push event ────│                     │
  │◀── event: message ───│                   │                     │
  │                      │                   │                     │
  │──── close ────────▶  │                   │                     │
  │               socket read error detected                       │
  │                      │── Unregister(X) ─▶│                     │
  │               exit thread                                      │
```

### Initialization handshake (session lifecycle)

1. Client sends `POST /mcp` with `{"method":"initialize",...}` (no `Mcp-Session-Id` yet).
2. Router detects `initialize`, calls `SessionRegistry.MintSession()` → `"9f1b..."`.
3. Response body includes the normal `initialize` result; response headers add `Mcp-Session-Id: 9f1b...`.
4. Client stores the id; every subsequent POST and the `GET /sse` carry `Mcp-Session-Id: 9f1b...`.
5. POST handler rejects requests with missing/unknown session id (except `initialize` itself) with `400 Bad Request`.
6. `GET /sse` registers itself under that id. Router and notifiers use it to reach the right stream.

### `notifications/progress` payload

Per MCP spec:
```json
{
  "jsonrpc": "2.0",
  "method": "notifications/progress",
  "params": {
    "progressToken": <token from request's params._meta.progressToken>,
    "progress": 0.5,
    "total": 1.0,
    "message": "Computing normals"
  }
}
```

Framed as SSE: `event: message\ndata: <the JSON above>\n\n`.

Final responses use the same event type with the JSON-RPC result as data.

## Error handling

### Socket-level

| Scenario | Behavior |
|---|---|
| Client drops mid-POST | Parser returns `EParseResult::Incomplete` after read timeout; connection closes silently. No handler dispatched. |
| Client drops mid-SSE response | Next socket write returns error; connection thread sets atomic `bClientGone`, exits keep-alive loop, unregisters from session registry, joins. In-flight background task still runs to completion (handlers are not cancellable mid-flight); its final write is dropped by the SSE writer. |
| Client drops mid-GET-SSE | Same as above. Session stays active for a reconnecting client. |
| Editor shutdown during active connections | `FSATcpServer::Stop()` sets global stop flag, closes listener, iterates active connections calling `RequestStop()` which shuts the socket. Each thread wakes with an error, cleans up, joins. Worst-case wait: one keep-alive tick (15 s). |
| `FSATcpServer::Stop()` while a handler is pumping the game thread queue | `FMCPGameThreadProcessor` is not disturbed. When the handler returns, its owning connection thread sees `bShuttingDown` and discards the response silently. |

### HTTP-level

| Scenario | Response |
|---|---|
| Unknown path | `404 Not Found`, `Content-Type: application/json`, body `{"error":"not found"}` |
| Bad verb for known path | `405 Method Not Allowed`, `Allow:` header set |
| Headers > 16 KiB | `431 Request Header Fields Too Large`, close |
| Body > 16 MiB | `413 Payload Too Large`, close |
| Malformed request line or headers | `400 Bad Request`, close |
| Missing `Content-Length` on POST with body | `411 Length Required` |
| Chunked request body | `501 Not Implemented`, "chunked request bodies unsupported" |
| Idle connection timeout (30 s waiting for request) | Close silently |
| 17th concurrent connection | `503 Service Unavailable`, close |

### MCP-level

| Scenario | Response |
|---|---|
| JSON parse failure on POST body | `200 OK` with JSON-RPC error `-32700 Parse error` (matches today) |
| `Mcp-Session-Id` missing on non-`initialize` POST | `400 Bad Request`, `{"error":"missing Mcp-Session-Id"}` |
| `Mcp-Session-Id` unknown | `404 Not Found`, suggesting re-running `initialize` |
| `Mcp-Session-Id` missing on `GET /sse` | `400 Bad Request` |
| `GET /sse` for a session with an already-active SSE stream | Close existing stream with `: session_replaced\n\n`, register the new one (last-writer-wins) |
| Client sent `Accept: application/json` only on a long op | Respond with single-body JSON. Client's problem if it times out — we honor what was asked for. |
| Handler throws / returns error | Same JSON-RPC error wrapping as today. In SSE mode, sent as `event: message` with the error payload. |

### `SendProgress` safety

- If the session id has no registered SSE stream, `SendProgress` is a silent no-op. Handlers always call without checking.
- Socket-write failure is swallowed silently by the writer; handler keeps running.
- `SendProgress` acquires the per-connection `SSEWriteLock` briefly — keep-alives and progress pushes can't interleave their SSE frames.

### Concurrency invariants

1. `FSASessionRegistry` is the only cross-thread mutable state. `FRWLock`: readers in parallel, writers serialize.
2. Each `FSAConnection` owns its `FSocket`. Only the connection thread writes, except via `SSEWriter::WriteEvent` which takes the per-connection mutex.
3. `FMCPGameThreadProcessor` and `FGameThreadDispatcher` are unchanged — their contract (must not be called from game thread) stands.
4. No callback into `FSAConnection` from the game thread. Notifications originate from the background task thread running the handler; they write to the socket directly under the per-connection mutex.

## Testing

UE plugins don't have a fast unit-test harness. Tests are a mix:

### 1. Static verification

- `grep`-based checks in a post-phase-4 commit that nothing under `Source/SpecialAgent` references `FHttpServerModule`, `FHttpServerResponse`, `FHttpRouter`, `FHttpRequestHandler`.
- Build succeeds — UBT reports it.

### 2. Curl-based integration smoke script

`docs/superpowers/specs/sse-transport-smoke.sh` — committed to the repo, runs against a live editor:

- `GET /health` → 200 + expected JSON shape.
- `POST /mcp initialize` → 200, records `Mcp-Session-Id`.
- `POST /mcp tools/list` with session id → 200, 300 tools.
- `POST /mcp tools/call level/get_current_path` with session id.
- `POST /mcp tools/call` with `Accept: text/event-stream` → 200, `Content-Type: text/event-stream`, terminates with `event: message` whose data is parseable JSON-RPC.
- `POST /mcp` missing session id → 400.
- `GET /sse` without session id → 400.
- `GET /sse` with session id in background; separate POST that emits progress; GET sees events within 2 s.
- `curl -I OPTIONS /mcp` → 204 with CORS headers.
- `POST /mcp` with 20 MiB body → 413.

Exits non-zero on any mismatch; intended to be run before a PR.

### 3. Parser unit tests

`Source/SpecialAgent/Private/Tests/SAHttpParserTests.cpp` (UE Automation):

- **Good:** basic GET; POST with body; OPTIONS; multiple headers; case-insensitive header lookup.
- **Bad:** missing Content-Length on POST body; chunked request body; oversized headers; malformed request line.
- **Edge:** CRLF vs LF line endings; header values with spaces/quotes; empty body.

These are the only pieces with enough isolated logic to justify unit testing. The rest is HTTP integration territory.

### 4. Manual Claude Code dogfood before merge

Attach Claude Code to the rebuilt plugin. Run three operations:
- Fast tool: `level/get_current_path` — sub-second.
- Medium tool: `screenshot/capture` — low seconds.
- Long tool: `material/*` normal compute — minutes.

All succeed. `initialize` returns a session id; subsequent calls reuse it.

## Rollout plan

Hard cutover per Decision 6. Commits on the `feature/sse-transport` branch, in order:

1. `feat(SpecialAgent/transport): scaffold FSATcpServer + parser + response writer` — new files; not yet wired.
2. `feat(SpecialAgent/transport): wire new transport on port 8768 side-by-side with HttpServerModule on 8767` — validate new.
3. `feat(SpecialAgent/transport): cut MCPServer over to raw TCP on 8767, remove HttpServerModule route setup` — old transport gone, Phase 1 complete.
4. `feat(SpecialAgent/transport): SSE response for POST + keep-alive (Phase 2)`.
5. `feat(SpecialAgent/transport): FMCPRequestContext + IMCPService signature bump across all services (Phase 3a)`.
6. `feat(SpecialAgent/transport): session registry + long-lived GET /sse + notifications/progress (Phase 3b)`.
7. `chore(SpecialAgent): drop HTTPServer module dependency + delete dead SSE placeholder (Phase 4)`.

Each commit compiles and the smoke script passes at its phase's acceptance bar. Abandoning a phase leaves earlier commits intact.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| HTTP parser edge cases bite in production | Parser unit tests + restricted scope (no chunked request body, no pipelining). Reject-and-log anything unexpected rather than guess. |
| Per-connection thread explosion under a bad client | Hard cap: 16 concurrent connections; 17th gets `503 Service Unavailable` + close. Editor use never approaches this. |
| `FTcpListener` platform bugs | Already widely used inside UE (LiveLink, network emulator); cross-platform surface is stable in 5.7. |
| 45-file `HandleRequest` signature bump typos | Single mechanical edit per file, caught at compile time. Parallel agent with a tight prompt handles the churn. |
| Cutover leaves editor unable to start HTTP | Rollout step 2 is side-by-side (8767 old + 8768 new); validate new on 8768, then step 3 switches ports and deletes old. |
| Session leak over long editor sessions | Sessions are small structs; no automatic expiry. Acceptable for a single-developer tool. Could be revisited later. |

## Acceptance gates per phase

| Phase | Acceptance test |
|---|---|
| 1 — Raw HTTP parity | `curl` smoke matrix (health, tools/list, 5 tools/call, initialize, ping, resources/templates/list) produces byte-identical responses to `main`. Full 300-tool `tools/list` round-trips. |
| 2 — POST SSE + keep-alive | `material/*` normal-map compute from Claude Code on a real asset completes without timeout. `tcpdump` shows periodic `: keepalive\n\n` frames during the wait. |
| 3 — GET /sse + progress | `curl -N http://localhost:8767/sse` in one shell; `tools/call` in another on a handler that emits `Ctx.SendProgress(...)`; GET stream receives `notifications/progress` events within 2 s. |
| 4 — Cleanup | `HTTPServer` not in `Build.cs`; no references to `FHttpServerModule`/`FHttpServerResponse`/`FHttpRouter` anywhere in `Source/SpecialAgent`. |

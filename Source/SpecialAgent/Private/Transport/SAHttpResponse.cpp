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
    // SSE: each newline in Data must become its own "data: " line; a blank line
    // terminates the event. Without this split, multiline payloads (e.g. pretty-
    // printed JSON) get truncated by the client at the first '\n'.
    FString SSE = FString::Printf(TEXT("event: %s\n"), *EventName);
    TArray<FString> Lines;
    Data.ParseIntoArray(Lines, TEXT("\n"), /*CullEmpty=*/false);
    for (FString& Line : Lines)
    {
        // Strip a trailing '\r' if the caller used CRLF separators.
        if (Line.EndsWith(TEXT("\r"))) Line.LeftChopInline(1);
        SSE += TEXT("data: ");
        SSE += Line;
        SSE += TEXT("\n");
    }
    SSE += TEXT("\n");   // blank line terminates the event
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

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

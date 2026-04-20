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

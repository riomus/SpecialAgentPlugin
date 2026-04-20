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

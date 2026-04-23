#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Transport/SAHttpParser.h"
#include "Transport/SATransportRouting.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSATransportRoutingCodexAliases,
    "SpecialAgent.Transport.Routing.CodexAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSATransportRoutingCodexAliases::RunTest(const FString&)
{
    TestTrue(TEXT("/codex is marked as the Codex compatibility route"),
        SATransportRouting::IsCodexCompatibilityRoute(TEXT("/codex")));
    TestFalse(TEXT("/mcp is not marked as the Codex compatibility route"),
        SATransportRouting::IsCodexCompatibilityRoute(TEXT("/mcp")));

    TestTrue(TEXT("POST /codex uses MCP handler"),
        SATransportRouting::IsMCPPostRoute(TEXT("POST"), TEXT("/codex")));
    TestTrue(TEXT("POST /mcp still uses MCP handler"),
        SATransportRouting::IsMCPPostRoute(TEXT("POST"), TEXT("/mcp")));
    TestFalse(TEXT("GET /codex does not use MCP POST handler"),
        SATransportRouting::IsMCPPostRoute(TEXT("GET"), TEXT("/codex")));

    TestTrue(TEXT("GET /codex uses SSE handler"),
        SATransportRouting::IsSSEGetRoute(TEXT("GET"), TEXT("/codex")));
    TestTrue(TEXT("GET /sse still uses SSE handler"),
        SATransportRouting::IsSSEGetRoute(TEXT("GET"), TEXT("/sse")));
    TestFalse(TEXT("POST /codex does not use SSE GET handler"),
        SATransportRouting::IsSSEGetRoute(TEXT("POST"), TEXT("/codex")));

    TestTrue(TEXT("/codex allows requests without Mcp-Session-Id"),
        SATransportRouting::AllowsOptionalSessionId(TEXT("/codex")));
    TestFalse(TEXT("/mcp still requires Mcp-Session-Id after initialize"),
        SATransportRouting::AllowsOptionalSessionId(TEXT("/mcp")));

    TestTrue(TEXT("notifications/initialized is treated as a notification"),
        SATransportRouting::IsNotificationMethod(TEXT("notifications/initialized")));
    TestTrue(TEXT("initialized is treated as a notification"),
        SATransportRouting::IsNotificationMethod(TEXT("initialized")));
    TestFalse(TEXT("initialize is not treated as a notification"),
        SATransportRouting::IsNotificationMethod(TEXT("initialize")));

    TestTrue(TEXT("/codex suppresses responses for notifications without ids"),
        SATransportRouting::ShouldSuppressResponse(TEXT("/codex"), TEXT("notifications/initialized"), TEXT("")));
    TestFalse(TEXT("/codex still responds to initialize"),
        SATransportRouting::ShouldSuppressResponse(TEXT("/codex"), TEXT("initialize"), TEXT("")));
    TestFalse(TEXT("/mcp keeps legacy notification responses"),
        SATransportRouting::ShouldSuppressResponse(TEXT("/mcp"), TEXT("notifications/initialized"), TEXT("")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

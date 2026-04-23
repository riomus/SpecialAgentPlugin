#include "Transport/SATransportRouting.h"

namespace SATransportRouting
{
	bool IsCodexCompatibilityRoute(const FString& Path)
	{
		return Path == TEXT("/codex");
	}

	bool IsMCPPostRoute(const FString& Verb, const FString& Path)
	{
		return Verb == TEXT("POST")
			&& (Path == TEXT("/mcp") || IsCodexCompatibilityRoute(Path));
	}

	bool IsSSEGetRoute(const FString& Verb, const FString& Path)
	{
		return Verb == TEXT("GET")
			&& (Path == TEXT("/sse") || IsCodexCompatibilityRoute(Path));
	}

	bool AllowsOptionalSessionId(const FString& Path)
	{
		return IsCodexCompatibilityRoute(Path);
	}

	bool IsNotificationMethod(const FString& Method)
	{
		return Method.StartsWith(TEXT("notifications/")) || Method == TEXT("initialized");
	}

	bool ShouldSuppressResponse(const FString& Path, const FString& Method, const FString& RequestId)
	{
		return IsCodexCompatibilityRoute(Path)
			&& RequestId.IsEmpty()
			&& IsNotificationMethod(Method);
	}
}

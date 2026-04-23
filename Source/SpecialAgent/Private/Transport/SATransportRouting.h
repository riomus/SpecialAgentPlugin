#pragma once

#include "CoreMinimal.h"

namespace SATransportRouting
{
	bool IsCodexCompatibilityRoute(const FString& Path);
	bool IsMCPPostRoute(const FString& Verb, const FString& Path);
	bool IsSSEGetRoute(const FString& Verb, const FString& Path);
	bool AllowsOptionalSessionId(const FString& Path);
	bool IsNotificationMethod(const FString& Method);
	bool ShouldSuppressResponse(const FString& Path, const FString& Method, const FString& RequestId);
}

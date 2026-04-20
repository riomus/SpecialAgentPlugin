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

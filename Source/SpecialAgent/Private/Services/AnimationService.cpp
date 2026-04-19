#include "Services/AnimationService.h"

FString FAnimationService::GetServiceDescription() const
{
    return TEXT("Skeletal mesh animation playback and anim blueprint assignment");
}

FMCPResponse FAnimationService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("animation"), MethodName);
}

TArray<FMCPToolInfo> FAnimationService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

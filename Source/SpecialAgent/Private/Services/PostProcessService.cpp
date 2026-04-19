#include "Services/PostProcessService.h"

FString FPostProcessService::GetServiceDescription() const
{
    return TEXT("Post-process volume spawn and tuning");
}

FMCPResponse FPostProcessService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("post_process"), MethodName);
}

TArray<FMCPToolInfo> FPostProcessService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

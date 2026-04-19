#include "Services/RenderQueueService.h"

FString FRenderQueueService::GetServiceDescription() const
{
    return TEXT("Movie Render Queue sequence rendering");
}

FMCPResponse FRenderQueueService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("render_queue"), MethodName);
}

TArray<FMCPToolInfo> FRenderQueueService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

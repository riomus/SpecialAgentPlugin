#include "Services/RenderingService.h"

FString FRenderingService::GetServiceDescription() const
{
    return TEXT("Scalability settings, view modes, Nanite / Lumen toggles");
}

FMCPResponse FRenderingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("rendering"), MethodName);
}

TArray<FMCPToolInfo> FRenderingService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

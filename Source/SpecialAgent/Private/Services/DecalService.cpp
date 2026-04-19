#include "Services/DecalService.h"

FString FDecalService::GetServiceDescription() const
{
    return TEXT("Decal actor spawn and configuration");
}

FMCPResponse FDecalService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("decal"), MethodName);
}

TArray<FMCPToolInfo> FDecalService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

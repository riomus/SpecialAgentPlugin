#include "Services/HLODService.h"

FString FHLODService::GetServiceDescription() const
{
    return TEXT("Hierarchical LOD build and management");
}

FMCPResponse FHLODService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("hlod"), MethodName);
}

TArray<FMCPToolInfo> FHLODService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

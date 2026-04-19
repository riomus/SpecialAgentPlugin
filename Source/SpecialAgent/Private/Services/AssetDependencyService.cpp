#include "Services/AssetDependencyService.h"

FString FAssetDependencyService::GetServiceDescription() const
{
    return TEXT("Query asset references and referencers");
}

FMCPResponse FAssetDependencyService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("asset_deps"), MethodName);
}

TArray<FMCPToolInfo> FAssetDependencyService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

#include "Services/AssetImportService.h"

FString FAssetImportService::GetServiceDescription() const
{
    return TEXT("Import FBX / textures / sounds / data tables");
}

FMCPResponse FAssetImportService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("asset_import"), MethodName);
}

TArray<FMCPToolInfo> FAssetImportService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

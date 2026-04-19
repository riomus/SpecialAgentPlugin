#include "Services/MaterialService.h"

FString FMaterialService::GetServiceDescription() const
{
    return TEXT("Material and material instance editing");
}

FMCPResponse FMaterialService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("material"), MethodName);
}

TArray<FMCPToolInfo> FMaterialService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

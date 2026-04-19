#include "Services/SkyService.h"

FString FSkyService::GetServiceDescription() const
{
    return TEXT("Sky atmosphere, height fog, volumetric cloud, sky light");
}

FMCPResponse FSkyService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("sky"), MethodName);
}

TArray<FMCPToolInfo> FSkyService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

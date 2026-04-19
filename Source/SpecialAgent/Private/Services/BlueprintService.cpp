#include "Services/BlueprintService.h"

FString FBlueprintService::GetServiceDescription() const
{
    return TEXT("Blueprint asset creation, compilation, and reflection");
}

FMCPResponse FBlueprintService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("blueprint"), MethodName);
}

TArray<FMCPToolInfo> FBlueprintService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

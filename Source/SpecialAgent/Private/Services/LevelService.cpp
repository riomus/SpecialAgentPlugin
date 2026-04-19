#include "Services/LevelService.h"

FString FLevelService::GetServiceDescription() const
{
    return TEXT("Open / new / save-as level files");
}

FMCPResponse FLevelService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("level"), MethodName);
}

TArray<FMCPToolInfo> FLevelService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

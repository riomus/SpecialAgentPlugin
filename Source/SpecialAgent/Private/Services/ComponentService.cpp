#include "Services/ComponentService.h"

FString FComponentService::GetServiceDescription() const
{
    return TEXT("Actor component add/remove/query/property editing");
}

FMCPResponse FComponentService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("component"), MethodName);
}

TArray<FMCPToolInfo> FComponentService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

#include "Services/ProjectService.h"

FString FProjectService::GetServiceDescription() const
{
    return TEXT("Project settings and plugin enablement");
}

FMCPResponse FProjectService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("project"), MethodName);
}

TArray<FMCPToolInfo> FProjectService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

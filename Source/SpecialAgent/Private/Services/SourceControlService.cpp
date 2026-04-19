#include "Services/SourceControlService.h"

FString FSourceControlService::GetServiceDescription() const
{
    return TEXT("Source control status, check-out, revert, submit");
}

FMCPResponse FSourceControlService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("source_control"), MethodName);
}

TArray<FMCPToolInfo> FSourceControlService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

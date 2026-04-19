#include "Services/LogService.h"

FString FLogService::GetServiceDescription() const
{
    return TEXT("Tail / clear the editor log and configure log categories");
}

FMCPResponse FLogService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("log"), MethodName);
}

TArray<FMCPToolInfo> FLogService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

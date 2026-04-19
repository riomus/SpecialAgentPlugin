#include "Services/ConsoleService.h"

FString FConsoleService::GetServiceDescription() const
{
    return TEXT("Execute console commands and manipulate CVars");
}

FMCPResponse FConsoleService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("console"), MethodName);
}

TArray<FMCPToolInfo> FConsoleService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

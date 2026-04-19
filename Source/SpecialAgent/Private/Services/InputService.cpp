#include "Services/InputService.h"

FString FInputService::GetServiceDescription() const
{
    return TEXT("Input mapping query and edit");
}

FMCPResponse FInputService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("input"), MethodName);
}

TArray<FMCPToolInfo> FInputService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

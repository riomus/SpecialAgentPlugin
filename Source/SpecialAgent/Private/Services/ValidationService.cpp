#include "Services/ValidationService.h"

FString FValidationService::GetServiceDescription() const
{
    return TEXT("Asset and level validation");
}

FMCPResponse FValidationService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("validation"), MethodName);
}

TArray<FMCPToolInfo> FValidationService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

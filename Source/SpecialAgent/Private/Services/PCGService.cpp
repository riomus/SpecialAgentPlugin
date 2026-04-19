#include "Services/PCGService.h"

FString FPCGService::GetServiceDescription() const
{
    return TEXT("Procedural Content Generation graph execution");
}

FMCPResponse FPCGService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("pcg"), MethodName);
}

TArray<FMCPToolInfo> FPCGService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

#include "Services/PhysicsService.h"

FString FPhysicsService::GetServiceDescription() const
{
    return TEXT("Physics simulation and body property control");
}

FMCPResponse FPhysicsService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("physics"), MethodName);
}

TArray<FMCPToolInfo> FPhysicsService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

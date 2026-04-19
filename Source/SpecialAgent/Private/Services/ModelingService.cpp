#include "Services/ModelingService.h"

FString FModelingService::GetServiceDescription() const
{
    return TEXT("Mesh boolean, extrude, simplify via Geometry Script");
}

FMCPResponse FModelingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("modeling"), MethodName);
}

TArray<FMCPToolInfo> FModelingService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

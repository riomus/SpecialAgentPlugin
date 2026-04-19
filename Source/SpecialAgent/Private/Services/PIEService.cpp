#include "Services/PIEService.h"

FString FPIEService::GetServiceDescription() const
{
    return TEXT("Play-In-Editor control (start/stop/pause/step)");
}

FMCPResponse FPIEService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("pie"), MethodName);
}

TArray<FMCPToolInfo> FPIEService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

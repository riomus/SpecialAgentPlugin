#include "Services/NiagaraService.h"

FString FNiagaraService::GetServiceDescription() const
{
    return TEXT("Niagara VFX spawning and parameter control");
}

FMCPResponse FNiagaraService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("niagara"), MethodName);
}

TArray<FMCPToolInfo> FNiagaraService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

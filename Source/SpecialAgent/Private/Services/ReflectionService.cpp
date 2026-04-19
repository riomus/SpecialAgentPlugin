#include "Services/ReflectionService.h"

FString FReflectionService::GetServiceDescription() const
{
    return TEXT("UObject / UClass / UProperty / UFunction introspection");
}

FMCPResponse FReflectionService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("reflection"), MethodName);
}

TArray<FMCPToolInfo> FReflectionService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

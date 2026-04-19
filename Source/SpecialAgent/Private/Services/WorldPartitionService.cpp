#include "Services/WorldPartitionService.h"

FString FWorldPartitionService::GetServiceDescription() const
{
    return TEXT("World Partition cell loading and streaming");
}

FMCPResponse FWorldPartitionService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("world_partition"), MethodName);
}

TArray<FMCPToolInfo> FWorldPartitionService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

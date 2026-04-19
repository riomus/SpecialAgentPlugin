#include "Services/DataTableService.h"

FString FDataTableService::GetServiceDescription() const
{
    return TEXT("Read and write data table rows");
}

FMCPResponse FDataTableService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("data_table"), MethodName);
}

TArray<FMCPToolInfo> FDataTableService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

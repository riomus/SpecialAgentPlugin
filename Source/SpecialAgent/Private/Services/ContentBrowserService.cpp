#include "Services/ContentBrowserService.h"

FString FContentBrowserService::GetServiceDescription() const
{
    return TEXT("Content Browser UI operations (sync, folders, metadata)");
}

FMCPResponse FContentBrowserService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("content_browser"), MethodName);
}

TArray<FMCPToolInfo> FContentBrowserService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

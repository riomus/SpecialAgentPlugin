#include "Services/EditorModeService.h"

FString FEditorModeService::GetServiceDescription() const
{
    return TEXT("Activate editor modes (landscape/foliage/modeling) and configure brushes");
}

FMCPResponse FEditorModeService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("editor_mode"), MethodName);
}

TArray<FMCPToolInfo> FEditorModeService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

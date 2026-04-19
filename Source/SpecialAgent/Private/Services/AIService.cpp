#include "Services/AIService.h"

FString FAIService::GetServiceDescription() const
{
    return TEXT("AI pawn / controller / behavior tree / blackboard");
}

FMCPResponse FAIService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("ai"), MethodName);
}

TArray<FMCPToolInfo> FAIService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

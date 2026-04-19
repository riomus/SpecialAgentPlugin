#include "Services/SoundService.h"

FString FSoundService::GetServiceDescription() const
{
    return TEXT("Sound playback and ambient sound actors");
}

FMCPResponse FSoundService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("sound"), MethodName);
}

TArray<FMCPToolInfo> FSoundService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

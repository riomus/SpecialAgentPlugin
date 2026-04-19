#include "Services/SequencerService.h"

FString FSequencerService::GetServiceDescription() const
{
    return TEXT("Level Sequence creation, bindings, tracks, keyframes, playback");
}

FMCPResponse FSequencerService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    return MethodNotFound(Request.Id, TEXT("sequencer"), MethodName);
}

TArray<FMCPToolInfo> FSequencerService::GetAvailableTools() const
{
    // Populated in Phase 1 — service currently advertises no tools.
    return {};
}

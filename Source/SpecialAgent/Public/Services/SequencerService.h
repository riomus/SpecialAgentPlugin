#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Sequencer Service.
 *
 * Level Sequence creation, bindings, tracks, keyframes, playback.
 */
class SPECIALAGENT_API FSequencerService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleCreate(const FMCPRequest& Request);
    FMCPResponse HandleAddActorBinding(const FMCPRequest& Request);
    FMCPResponse HandleAddTransformTrack(const FMCPRequest& Request);
    FMCPResponse HandleAddKeyframe(const FMCPRequest& Request);
    FMCPResponse HandleSetPlaybackRange(const FMCPRequest& Request);
    FMCPResponse HandlePlay(const FMCPRequest& Request);
};

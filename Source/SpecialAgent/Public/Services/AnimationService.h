#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Animation Service.
 *
 * Skeletal mesh animation playback and anim blueprint assignment.
 */
class SPECIALAGENT_API FAnimationService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandlePlay(const FMCPRequest& Request);
    FMCPResponse HandleStop(const FMCPRequest& Request);
    FMCPResponse HandleSetAnimBlueprint(const FMCPRequest& Request);
    FMCPResponse HandleListAnimations(const FMCPRequest& Request);
    FMCPResponse HandleSetPose(const FMCPRequest& Request);
};

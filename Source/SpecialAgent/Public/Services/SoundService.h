#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Sound Service.
 *
 * Sound playback (2D / at location) and ambient sound actors.
 */
class SPECIALAGENT_API FSoundService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandlePlay2D(const FMCPRequest& Request);
    FMCPResponse HandlePlayAtLocation(const FMCPRequest& Request);
    FMCPResponse HandleSpawnAmbientActor(const FMCPRequest& Request);
    FMCPResponse HandleSetVolumeMultiplier(const FMCPRequest& Request);
};

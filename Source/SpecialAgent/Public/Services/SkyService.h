#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Sky Service.
 *
 * Spawn sky actors (SkyAtmosphere, ExponentialHeightFog, VolumetricCloud, SkyLight)
 * and control the sun angle on an ADirectionalLight via a time-of-day helper.
 */
class SPECIALAGENT_API FSkyService : public IMCPService
{
public:
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSpawnSkyAtmosphere(const FMCPRequest& Request);
	FMCPResponse HandleSpawnHeightFog(const FMCPRequest& Request);
	FMCPResponse HandleSpawnCloud(const FMCPRequest& Request);
	FMCPResponse HandleSpawnSkyLight(const FMCPRequest& Request);
	FMCPResponse HandleSetSunAngle(const FMCPRequest& Request);
};

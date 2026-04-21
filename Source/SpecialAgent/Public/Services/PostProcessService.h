#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * PostProcess Service.
 *
 * Spawn and tune APostProcessVolume actors (exposure, bloom, DOF,
 * color grading, GI) via UPROPERTY overrides on FPostProcessSettings.
 */
class SPECIALAGENT_API FPostProcessService : public IMCPService
{
public:
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSpawnVolume(const FMCPRequest& Request);
	FMCPResponse HandleSetExposure(const FMCPRequest& Request);
	FMCPResponse HandleSetBloom(const FMCPRequest& Request);
	FMCPResponse HandleSetDof(const FMCPRequest& Request);
	FMCPResponse HandleSetColorGrading(const FMCPRequest& Request);
	FMCPResponse HandleSetGi(const FMCPRequest& Request);
};

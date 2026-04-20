#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Decal Service.
 *
 * Spawn ADecalActor and configure its material and projected size.
 */
class SPECIALAGENT_API FDecalService : public IMCPService
{
public:
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSpawn(const FMCPRequest& Request);
	FMCPResponse HandleSetMaterial(const FMCPRequest& Request);
	FMCPResponse HandleSetSize(const FMCPRequest& Request);
};

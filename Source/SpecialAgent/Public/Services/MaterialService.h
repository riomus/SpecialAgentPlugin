#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Material Service.
 *
 * Create materials and material instance constants, edit scalar/vector/texture
 * and static-switch parameters, and introspect the parameter list of any
 * UMaterialInterface. See spec §Tool Catalog for the authoritative list.
 */
class SPECIALAGENT_API FMaterialService : public IMCPService
{
public:
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleCreate(const FMCPRequest& Request);
	FMCPResponse HandleCreateInstance(const FMCPRequest& Request);
	FMCPResponse HandleSetScalarParameter(const FMCPRequest& Request);
	FMCPResponse HandleSetVectorParameter(const FMCPRequest& Request);
	FMCPResponse HandleSetTextureParameter(const FMCPRequest& Request);
	FMCPResponse HandleSetStaticSwitch(const FMCPRequest& Request);
	FMCPResponse HandleListParameters(const FMCPRequest& Request);
	FMCPResponse HandleGetParameters(const FMCPRequest& Request);
};

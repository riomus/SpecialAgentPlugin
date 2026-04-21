#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Input Service.
 *
 * Input mapping query and edit for both the legacy UInputSettings action/axis
 * mappings and the modern Enhanced Input system (UInputAction +
 * UInputMappingContext assets).
 */
class SPECIALAGENT_API FInputService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    // Legacy UInputSettings
    FMCPResponse HandleListMappings(const FMCPRequest& Request);
    FMCPResponse HandleAddActionMapping(const FMCPRequest& Request);
    FMCPResponse HandleAddAxisMapping(const FMCPRequest& Request);
    FMCPResponse HandleRemoveMapping(const FMCPRequest& Request);

    // Enhanced Input (UInputAction / UInputMappingContext assets)
    FMCPResponse HandleListEnhancedActions(const FMCPRequest& Request);
    FMCPResponse HandleListMappingContexts(const FMCPRequest& Request);
    FMCPResponse HandleGetMappingContext(const FMCPRequest& Request);
    FMCPResponse HandleAddEnhancedMapping(const FMCPRequest& Request);
    FMCPResponse HandleRemoveEnhancedMapping(const FMCPRequest& Request);
};

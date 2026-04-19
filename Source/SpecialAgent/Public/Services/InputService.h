#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Input Service.
 *
 * Input mapping query and edit (legacy UInputSettings action/axis mappings).
 */
class SPECIALAGENT_API FInputService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleListMappings(const FMCPRequest& Request);
    FMCPResponse HandleAddActionMapping(const FMCPRequest& Request);
    FMCPResponse HandleAddAxisMapping(const FMCPRequest& Request);
    FMCPResponse HandleRemoveMapping(const FMCPRequest& Request);
};

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * HLOD Service.
 *
 * Hierarchical LOD build and management.
 * Methods: build, clear, set_setting.
 */
class SPECIALAGENT_API FHLODService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleBuild(const FMCPRequest& Request);
    FMCPResponse HandleClear(const FMCPRequest& Request);
    FMCPResponse HandleSetSetting(const FMCPRequest& Request);
};

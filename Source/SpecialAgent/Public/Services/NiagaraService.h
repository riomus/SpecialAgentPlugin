#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Niagara Service.
 *
 * Niagara VFX spawning and parameter control.
 */
class SPECIALAGENT_API FNiagaraService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleSpawnEmitter(const FMCPRequest& Request);
    FMCPResponse HandleSetParameter(const FMCPRequest& Request);
    FMCPResponse HandleActivate(const FMCPRequest& Request);
    FMCPResponse HandleDeactivate(const FMCPRequest& Request);
    FMCPResponse HandleSetUserFloat(const FMCPRequest& Request);
    FMCPResponse HandleSetUserVec3(const FMCPRequest& Request);
};

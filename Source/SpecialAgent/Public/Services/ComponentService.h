#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Component Service.
 *
 * Actor component add/remove/query/property editing.
 */
class SPECIALAGENT_API FComponentService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleAdd(const FMCPRequest& Request);
    FMCPResponse HandleRemove(const FMCPRequest& Request);
    FMCPResponse HandleList(const FMCPRequest& Request);
    FMCPResponse HandleGetProperties(const FMCPRequest& Request);
    FMCPResponse HandleSetProperty(const FMCPRequest& Request);
    FMCPResponse HandleAttach(const FMCPRequest& Request);
    FMCPResponse HandleDetach(const FMCPRequest& Request);
};

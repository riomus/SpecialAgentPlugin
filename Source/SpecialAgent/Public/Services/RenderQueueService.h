#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * RenderQueue Service.
 *
 * Movie Render Queue sequence rendering.
 */
class SPECIALAGENT_API FRenderQueueService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleQueueSequence(const FMCPRequest& Request);
    FMCPResponse HandleSetOutput(const FMCPRequest& Request);
    FMCPResponse HandleGetStatus(const FMCPRequest& Request);
};

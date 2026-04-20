#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Rendering Service.
 *
 * Scalability settings, view modes, Nanite / Lumen toggles, high-res screenshot.
 */
class SPECIALAGENT_API FRenderingService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleSetScalability(const FMCPRequest& Request);
    FMCPResponse HandleSetViewMode(const FMCPRequest& Request);
    FMCPResponse HandleHighResScreenshot(const FMCPRequest& Request);
    FMCPResponse HandleToggleNanite(const FMCPRequest& Request);
    FMCPResponse HandleToggleLumen(const FMCPRequest& Request);
};

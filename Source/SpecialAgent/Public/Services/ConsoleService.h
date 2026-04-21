#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Console Service.
 *
 * Execute console commands and manipulate CVars.
 *
 * Tool list will be populated in Phase 1. See
 * docs/superpowers/plans/2026-04-19-ue5-mcp-tools-expansion-plan.md
 * and docs/superpowers/specs/2026-04-19-ue5-mcp-tools-expansion-design.md
 * for the catalog of tools this service owns.
 */
class SPECIALAGENT_API FConsoleService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;
};

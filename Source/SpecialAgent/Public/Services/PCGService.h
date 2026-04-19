#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * PCG Service.
 *
 * Procedural Content Generation graph execution.
 * Methods: list_graphs, execute_graph, spawn_pcg_actor.
 */
class SPECIALAGENT_API FPCGService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleListGraphs(const FMCPRequest& Request);
    FMCPResponse HandleExecuteGraph(const FMCPRequest& Request);
    FMCPResponse HandleSpawnPCGActor(const FMCPRequest& Request);
};

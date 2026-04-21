#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * AI Service.
 *
 * AI pawn / controller / behavior tree / blackboard.
 */
class SPECIALAGENT_API FAIService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleSpawnAIPawn(const FMCPRequest& Request);
    FMCPResponse HandleAssignController(const FMCPRequest& Request);
    FMCPResponse HandleRunBehaviorTree(const FMCPRequest& Request);
    FMCPResponse HandleSetBlackboardValue(const FMCPRequest& Request);
    FMCPResponse HandleStopAI(const FMCPRequest& Request);
};

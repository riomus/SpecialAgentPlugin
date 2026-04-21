#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Modeling Service.
 *
 * Mesh boolean, extrude, simplify via Geometry Script.
 * Methods: boolean_union, boolean_subtract, extrude, simplify.
 */
class SPECIALAGENT_API FModelingService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleBooleanUnion(const FMCPRequest& Request);
    FMCPResponse HandleBooleanSubtract(const FMCPRequest& Request);
    FMCPResponse HandleExtrude(const FMCPRequest& Request);
    FMCPResponse HandleSimplify(const FMCPRequest& Request);
};

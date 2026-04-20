#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Physics Service.
 *
 * Physics simulation and body property control on an actor's primary UPrimitiveComponent.
 */
class SPECIALAGENT_API FPhysicsService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleSetSimulatePhysics(const FMCPRequest& Request);
    FMCPResponse HandleApplyImpulse(const FMCPRequest& Request);
    FMCPResponse HandleApplyForce(const FMCPRequest& Request);
    FMCPResponse HandleSetLinearVelocity(const FMCPRequest& Request);
    FMCPResponse HandleSetAngularVelocity(const FMCPRequest& Request);
    FMCPResponse HandleSetMass(const FMCPRequest& Request);
    FMCPResponse HandleSetCollisionEnabled(const FMCPRequest& Request);
};

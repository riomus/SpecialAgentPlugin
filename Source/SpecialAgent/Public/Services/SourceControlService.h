#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * SourceControl Service.
 *
 * Source control status, check-out, revert, submit.
 * Methods: get_status, check_out, revert, submit, list_modified.
 */
class SPECIALAGENT_API FSourceControlService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleGetStatus(const FMCPRequest& Request);
    FMCPResponse HandleCheckOut(const FMCPRequest& Request);
    FMCPResponse HandleRevert(const FMCPRequest& Request);
    FMCPResponse HandleSubmit(const FMCPRequest& Request);
    FMCPResponse HandleListModified(const FMCPRequest& Request);
};

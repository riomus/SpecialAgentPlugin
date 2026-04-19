#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Blueprint Service.
 *
 * Blueprint asset creation, compilation, and reflection.
 *
 * Implements 10 tools: create, compile, add_variable, add_function,
 * set_default_value, list_functions, list_variables, open_in_editor,
 * duplicate, reparent.
 */
class SPECIALAGENT_API FBlueprintService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleCreate(const FMCPRequest& Request);
    FMCPResponse HandleCompile(const FMCPRequest& Request);
    FMCPResponse HandleAddVariable(const FMCPRequest& Request);
    FMCPResponse HandleAddFunction(const FMCPRequest& Request);
    FMCPResponse HandleSetDefaultValue(const FMCPRequest& Request);
    FMCPResponse HandleListFunctions(const FMCPRequest& Request);
    FMCPResponse HandleListVariables(const FMCPRequest& Request);
    FMCPResponse HandleOpenInEditor(const FMCPRequest& Request);
    FMCPResponse HandleDuplicate(const FMCPRequest& Request);
    FMCPResponse HandleReparent(const FMCPRequest& Request);
};

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Reflection Service.
 *
 * UObject / UClass / UProperty / UFunction introspection.
 *
 * Implements 5 tools: list_classes, get_class_info, list_properties,
 * list_functions, call_function (primitive args only).
 */
class SPECIALAGENT_API FReflectionService : public IMCPService
{
public:
    virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
    virtual FString GetServiceDescription() const override;
    virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
    FMCPResponse HandleListClasses(const FMCPRequest& Request);
    FMCPResponse HandleGetClassInfo(const FMCPRequest& Request);
    FMCPResponse HandleListProperties(const FMCPRequest& Request);
    FMCPResponse HandleListFunctions(const FMCPRequest& Request);
    FMCPResponse HandleCallFunction(const FMCPRequest& Request);
};

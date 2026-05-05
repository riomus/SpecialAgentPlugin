// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Python Service
 * 
 * PRIMARY CONTROL MECHANISM - Execute Python scripts with full UE5 API access.
 * Methods: execute, execute_file, list_modules
 */
class SPECIALAGENT_API FPythonService : public IMCPService
{
public:
	FPythonService();
	virtual ~FPythonService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

	// Public API for other services to execute Python code
	FMCPResponse HandleExecute(const FMCPRequest& Request);

private:
	FMCPResponse HandleExecuteFile(const FMCPRequest& Request);
	FMCPResponse HandleListModules(const FMCPRequest& Request);

	// Live introspection of the running 'unreal' module (game-thread Python).
	FMCPResponse HandleHelp(const FMCPRequest& Request);
	FMCPResponse HandleInspectClass(const FMCPRequest& Request);
	FMCPResponse HandleListSubsystems(const FMCPRequest& Request);
	FMCPResponse HandleSearchSymbol(const FMCPRequest& Request);
	FMCPResponse HandleGetFunctionSignature(const FMCPRequest& Request);
	FMCPResponse HandleListEnumValues(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetClassForPath(const FMCPRequest& Request);

	// Pure C++ scan of an input snippet against deprecations.md (no Python).
	FMCPResponse HandleDiffAgainstDeprecated(const FMCPRequest& Request);
};


// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPServer.h"
#include "MCPCommon/MCPRequestContext.h"

class IMCPService;

/**
 * MCP Request Router
 * 
 * Routes incoming MCP requests to the appropriate service handlers.
 * Manages registration and lookup of services.
 */
class SPECIALAGENT_API FMCPRequestRouter
{
public:
	FMCPRequestRouter();
	~FMCPRequestRouter();

	/**
	 * Route a request to the appropriate service
	 * @param Request The incoming MCP request
	 * @param Ctx Request-scoped context (session id, progress callback, etc.)
	 * @return The MCP response
	 */
	FMCPResponse RouteRequest(const FMCPRequest& Request, const FMCPRequestContext& Ctx);

	/**
	 * Register a service handler
	 * @param ServicePrefix The method prefix for this service (e.g., "assets", "world")
	 * @param Service The service implementation
	 */
	void RegisterService(const FString& ServicePrefix, TSharedPtr<IMCPService> Service);

	/**
	 * Log one line per registered service summarizing its tool count, and
	 * emit a Warning for any service that is registered but exposes zero
	 * tools. Call from the router constructor (after RegisterService calls).
	 */
	void ValidateServices() const;

private:
	/** Handle MCP initialize request */
	FMCPResponse HandleInitialize(const FMCPRequest& Request);
	
	/** Handle tools/list request */
	FMCPResponse HandleToolsList(const FMCPRequest& Request);
	
	/** Handle tools/call request */
	FMCPResponse HandleToolsCall(const FMCPRequest& Request, const FMCPRequestContext& Ctx);
	
	/** Handle server info request */
	FMCPResponse HandleServerInfo(const FMCPRequest& Request);
	
	/** Handle getInstructions request - returns server instructions */
	FMCPResponse HandleGetInstructions(const FMCPRequest& Request);
	
	/** Handle resources/list request - returns available resources */
	FMCPResponse HandleResourcesList(const FMCPRequest& Request);
	
	/** Handle resources/read request - returns resource content */
	FMCPResponse HandleResourcesRead(const FMCPRequest& Request);
	
	/** Handle prompts/list request - returns available prompts */
	FMCPResponse HandlePromptsList(const FMCPRequest& Request);
	
	/** Handle prompts/get request - returns a specific prompt */
	FMCPResponse HandlePromptsGet(const FMCPRequest& Request);
	
	/** Wrap a service response in MCP content format */
	FMCPResponse WrapToolResponse(const FMCPResponse& ServiceResponse, const FString& ServicePrefix, const FString& MethodName);

	/** Registered services */
	TMap<FString, TSharedPtr<IMCPService>> Services;
};


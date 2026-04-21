// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FMCPRequestRouter;
struct FMCPRequest;
struct FMCPResponse;
class FSATcpServer;

/**
 * MCP Server Implementation
 *
 * Owns the raw-TCP transport (FSATcpServer) and the request router.
 * Handles incoming requests from MCP clients (Claude Code, Cursor, custom)
 * over HTTP/1.1 + SSE on a single port.
 */
class SPECIALAGENT_API FSpecialAgentMCPServer
{
public:
	FSpecialAgentMCPServer();
	~FSpecialAgentMCPServer();

	/**
	 * Start the MCP server on the specified port
	 * @param Port The port to listen on (default 8767)
	 * @return true if server started successfully
	 */
	bool StartServer(int32 Port = 8767);

	/**
	 * Stop the MCP server
	 */
	void StopServer();

	/** Check if the server is running */
	bool IsRunning() const { return bIsRunning; }

	/** Get the request router */
	TSharedPtr<FMCPRequestRouter> GetRouter() const { return RequestRouter; }

	/** Number of recently connected clients (based on recent request activity). */
	int32 GetConnectedClientCount() const;

	/** Record a client request (called by the transport to track activity). */
	void RecordClientActivity();

	/** Parse JSON-RPC request from body (static — used by the raw-TCP transport). */
	static bool ParseRequest(const FString& JsonString, FMCPRequest& OutRequest);

	/** Format JSON-RPC response (static — used by the raw-TCP transport). */
	static FString FormatResponse(const FMCPResponse& Response);

private:
	/** Raw-TCP transport (FTcpListener + FSAConnection). Owns the listening socket. */
	TUniquePtr<FSATcpServer> RawServer;

	/** Request router (shared with RawServer). */
	TSharedPtr<FMCPRequestRouter> RequestRouter;

	/** Server running flag. */
	bool bIsRunning;

	/** Server port (from StartServer). */
	int32 ServerPort;

	/** Last time we received a request from a client. */
	FDateTime LastClientActivity;

	/** Consider client "connected" if activity within this many seconds. */
	static constexpr double ClientActivityTimeoutSeconds = 30.0;
};


/**
 * MCP Request Structure (JSON-RPC 2.0).
 */
struct FMCPRequest
{
	FString JsonRpc;  // Should be "2.0"
	FString Method;
	TSharedPtr<FJsonObject> Params;
	FString Id;  // Can be string or number

	FMCPRequest() : JsonRpc(TEXT("2.0")) {}
};


/**
 * MCP Response Structure (JSON-RPC 2.0).
 */
struct FMCPResponse
{
	FString JsonRpc;  // Should be "2.0"
	TSharedPtr<FJsonObject> Result;
	TSharedPtr<FJsonObject> ErrorObject;
	FString Id;

	bool bSuccess;

	FMCPResponse() : JsonRpc(TEXT("2.0")), bSuccess(true) {}

	/** Create success response */
	static FMCPResponse Success(const FString& InId, TSharedPtr<FJsonObject> InResult)
	{
		FMCPResponse Response;
		Response.Id = InId;
		Response.Result = InResult;
		Response.bSuccess = true;
		return Response;
	}

	/** Create error response */
	static FMCPResponse Error(const FString& InId, int32 ErrorCode, const FString& ErrorMessage, TSharedPtr<FJsonObject> ErrorData = nullptr)
	{
		FMCPResponse Response;
		Response.Id = InId;
		Response.bSuccess = false;

		TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetNumberField(TEXT("code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("message"), ErrorMessage);
		if (ErrorData.IsValid())
		{
			ErrorObj->SetObjectField(TEXT("data"), ErrorData);
		}

		Response.ErrorObject = ErrorObj;
		return Response;
	}
};

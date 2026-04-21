// Copyright Epic Games, Inc. All Rights Reserved.

#include "MCPServer.h"
#include "MCPRequestRouter.h"
#include "Transport/SATcpServer.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FSpecialAgentMCPServer::FSpecialAgentMCPServer()
	: bIsRunning(false)
	, ServerPort(8767)
	, LastClientActivity(FDateTime::MinValue())
{
	RequestRouter = MakeShared<FMCPRequestRouter>();
}

FSpecialAgentMCPServer::~FSpecialAgentMCPServer()
{
	StopServer();
}

bool FSpecialAgentMCPServer::StartServer(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: MCP Server is already running"));
		return false;
	}

	ServerPort = Port;

	RawServer = MakeUnique<FSATcpServer>(RequestRouter);
	if (!RawServer->Start(ServerPort))
	{
		UE_LOG(LogTemp, Error, TEXT("SpecialAgent: raw TCP transport failed to bind %d"), ServerPort);
		RawServer.Reset();
		return false;
	}

	bIsRunning = true;
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: MCP server started on port %d (raw TCP transport)"), ServerPort);
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: POST http://localhost:%d/mcp — JSON-RPC 2.0 endpoint"), ServerPort);
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: GET  http://localhost:%d/sse — long-lived notifications stream"), ServerPort);
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: GET  http://localhost:%d/health — debug probe"), ServerPort);
	return true;
}

void FSpecialAgentMCPServer::StopServer()
{
	if (!bIsRunning) return;
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: MCP server stopping"));
	if (RawServer) { RawServer->Stop(); RawServer.Reset(); }
	bIsRunning = false;
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: MCP server stopped"));
}

bool FSpecialAgentMCPServer::ParseRequest(const FString& JsonString, FMCPRequest& OutRequest)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return false;
	}

	OutRequest.JsonRpc = JsonObject->GetStringField(TEXT("jsonrpc"));
	OutRequest.Method  = JsonObject->GetStringField(TEXT("method"));

	const TSharedPtr<FJsonObject>* ParamsObj;
	if (JsonObject->TryGetObjectField(TEXT("params"), ParamsObj))
	{
		OutRequest.Params = *ParamsObj;
	}
	else
	{
		OutRequest.Params = MakeShared<FJsonObject>();
	}

	const TSharedPtr<FJsonValue> IdValue = JsonObject->TryGetField(TEXT("id"));
	if (IdValue.IsValid())
	{
		if (IdValue->Type == EJson::String)
		{
			OutRequest.Id = IdValue->AsString();
		}
		else if (IdValue->Type == EJson::Number)
		{
			OutRequest.Id = FString::Printf(TEXT("%d"), (int32)IdValue->AsNumber());
		}
	}
	return true;
}

FString FSpecialAgentMCPServer::FormatResponse(const FMCPResponse& Response)
{
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
	JsonObject->SetStringField(TEXT("jsonrpc"), Response.JsonRpc);

	if (!Response.Id.IsEmpty())
	{
		if (Response.Id.IsNumeric())
		{
			JsonObject->SetNumberField(TEXT("id"), FCString::Atoi(*Response.Id));
		}
		else
		{
			JsonObject->SetStringField(TEXT("id"), Response.Id);
		}
	}
	else
	{
		JsonObject->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}

	if (Response.bSuccess && Response.Result.IsValid())
	{
		JsonObject->SetObjectField(TEXT("result"), Response.Result);
	}
	else if (!Response.bSuccess && Response.ErrorObject.IsValid())
	{
		JsonObject->SetObjectField(TEXT("error"), Response.ErrorObject);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

int32 FSpecialAgentMCPServer::GetConnectedClientCount() const
{
	if (!bIsRunning) return 0;
	FTimespan TimeSinceActivity = FDateTime::Now() - LastClientActivity;
	if (TimeSinceActivity.GetTotalSeconds() < ClientActivityTimeoutSeconds)
	{
		return 1;
	}
	return 0;
}

void FSpecialAgentMCPServer::RecordClientActivity()
{
	LastClientActivity = FDateTime::Now();
}

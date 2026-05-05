// Copyright Epic Games, Inc. All Rights Reserved.

#include "MCPRequestRouter.h"
#include "Services/IMCPService.h"
#include "Services/AssetService.h"
#include "Services/WorldService.h"
#include "Services/PythonService.h"
#include "Services/ViewportService.h"
#include "Services/ScreenshotService.h"
#include "Services/LightingService.h"
#include "Services/FoliageService.h"
#include "Services/LandscapeService.h"
#include "Services/StreamingService.h"
#include "Services/PerformanceService.h"
#include "Services/NavigationService.h"
#include "Services/GameplayService.h"
#include "Services/UtilityService.h"
#include "Services/BlueprintService.h"
#include "Services/MaterialService.h"
#include "Services/AssetImportService.h"
#include "Services/PIEService.h"
#include "Services/ConsoleService.h"
#include "Services/ComponentService.h"
#include "Services/EditorModeService.h"
#include "Services/LevelService.h"
#include "Services/LogService.h"
#include "Services/DataTableService.h"
#include "Services/AssetDependencyService.h"
#include "Services/SequencerService.h"
#include "Services/NiagaraService.h"
#include "Services/SoundService.h"
#include "Services/WorldPartitionService.h"
#include "Services/PCGService.h"
#include "Services/ContentBrowserService.h"
#include "Services/ProjectService.h"
#include "Services/ReflectionService.h"
#include "Services/PhysicsService.h"
#include "Services/AnimationService.h"
#include "Services/AIService.h"
#include "Services/PostProcessService.h"
#include "Services/SkyService.h"
#include "Services/DecalService.h"
#include "Services/HLODService.h"
#include "Services/RenderingService.h"
#include "Services/ValidationService.h"
#include "Services/SourceControlService.h"
#include "Services/RenderQueueService.h"
#include "Services/ModelingService.h"
#include "Services/InputService.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace
{
	static FString GetSpecialAgentDocsDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetContentDir(), TEXT("Docs"));
	}

	static FString BuildSpecialAgentInstructions()
	{
		static FString Cached;
		static bool bLoaded = false;
		if (bLoaded) return Cached;

		auto FallbackHardcoded = []() -> FString
		{
			return TEXT(
				"SpecialAgent controls Unreal Editor via MCP tools. "
				"See Content/Docs/ue5_python_cheatsheet.md (missing on disk — using fallback). "
				"WORKFLOW: screenshot/capture -> inspect/select/trace -> act -> screenshot/capture. "
				"After camera changes, call viewport/force_redraw before screenshot. "
				"Prefer specific service tools; fall back to python/execute for the long tail. "
				"Use unreal.get_editor_subsystem(unreal.EditorActorSubsystem) — NOT deprecated EditorLevelLibrary."
			);
		};

		const FString DocsDir = GetSpecialAgentDocsDir();
		if (DocsDir.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: plugin not found, using fallback instructions"));
			Cached = FallbackHardcoded();
			bLoaded = true;
			return Cached;
		}

		const FString CheatSheetPath = FPaths::Combine(DocsDir, TEXT("ue5_python_cheatsheet.md"));
		if (!FFileHelper::LoadFileToString(Cached, *CheatSheetPath))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("SpecialAgent: cheat sheet not found at %s — using fallback"), *CheatSheetPath);
			Cached = FallbackHardcoded();
		}
		else
		{
			UE_LOG(LogTemp, Log,
				TEXT("SpecialAgent: loaded instructions cheat sheet (%d bytes) from %s"),
				Cached.Len(), *CheatSheetPath);
		}

		bLoaded = true;
		return Cached;
	}
}

FMCPRequestRouter::FMCPRequestRouter()
{
	// Register all services
	RegisterService(TEXT("assets"), MakeShared<FAssetService>());
	RegisterService(TEXT("world"), MakeShared<FWorldService>());
	RegisterService(TEXT("python"), MakeShared<FPythonService>());
	RegisterService(TEXT("viewport"), MakeShared<FViewportService>());
	RegisterService(TEXT("screenshot"), MakeShared<FScreenshotService>());
	RegisterService(TEXT("lighting"), MakeShared<FLightingService>());
	RegisterService(TEXT("foliage"), MakeShared<FFoliageService>());
	RegisterService(TEXT("landscape"), MakeShared<FLandscapeService>());
	RegisterService(TEXT("streaming"), MakeShared<FStreamingService>());
	RegisterService(TEXT("performance"), MakeShared<FPerformanceService>());
	RegisterService(TEXT("navigation"), MakeShared<FNavigationService>());
	RegisterService(TEXT("gameplay"), MakeShared<FGameplayService>());
	RegisterService(TEXT("utility"), MakeShared<FUtilityService>());

	// Phase 0.6 scaffolded services — handlers/tools populated in Phase 1.
	RegisterService(TEXT("blueprint"),       MakeShared<FBlueprintService>());
	RegisterService(TEXT("material"),        MakeShared<FMaterialService>());
	RegisterService(TEXT("asset_import"),    MakeShared<FAssetImportService>());
	RegisterService(TEXT("pie"),             MakeShared<FPIEService>());
	RegisterService(TEXT("console"),         MakeShared<FConsoleService>());
	RegisterService(TEXT("component"),       MakeShared<FComponentService>());
	RegisterService(TEXT("editor_mode"),     MakeShared<FEditorModeService>());
	RegisterService(TEXT("level"),           MakeShared<FLevelService>());
	RegisterService(TEXT("log"),             MakeShared<FLogService>());
	RegisterService(TEXT("data_table"),      MakeShared<FDataTableService>());
	RegisterService(TEXT("asset_deps"),      MakeShared<FAssetDependencyService>());
	RegisterService(TEXT("sequencer"),       MakeShared<FSequencerService>());
	RegisterService(TEXT("niagara"),         MakeShared<FNiagaraService>());
	RegisterService(TEXT("sound"),           MakeShared<FSoundService>());
	RegisterService(TEXT("world_partition"), MakeShared<FWorldPartitionService>());
	RegisterService(TEXT("pcg"),             MakeShared<FPCGService>());
	RegisterService(TEXT("content_browser"), MakeShared<FContentBrowserService>());
	RegisterService(TEXT("project"),         MakeShared<FProjectService>());
	RegisterService(TEXT("reflection"),      MakeShared<FReflectionService>());
	RegisterService(TEXT("physics"),         MakeShared<FPhysicsService>());
	RegisterService(TEXT("animation"),       MakeShared<FAnimationService>());
	RegisterService(TEXT("ai"),              MakeShared<FAIService>());
	RegisterService(TEXT("post_process"),    MakeShared<FPostProcessService>());
	RegisterService(TEXT("sky"),             MakeShared<FSkyService>());
	RegisterService(TEXT("decal"),           MakeShared<FDecalService>());
	RegisterService(TEXT("hlod"),            MakeShared<FHLODService>());
	RegisterService(TEXT("rendering"),       MakeShared<FRenderingService>());
	RegisterService(TEXT("validation"),      MakeShared<FValidationService>());
	RegisterService(TEXT("source_control"),  MakeShared<FSourceControlService>());
	RegisterService(TEXT("render_queue"),    MakeShared<FRenderQueueService>());
	RegisterService(TEXT("modeling"),        MakeShared<FModelingService>());
	RegisterService(TEXT("input"),           MakeShared<FInputService>());

	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Registered %d services"), Services.Num());
	ValidateServices();
}

FMCPRequestRouter::~FMCPRequestRouter()
{
}

void FMCPRequestRouter::ValidateServices() const
{
	int32 TotalTools = 0;
	int32 DeadServices = 0;
	for (const auto& Pair : Services)
	{
		const TArray<FMCPToolInfo> Tools = Pair.Value->GetAvailableTools();
		if (Tools.Num() == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("SpecialAgent: service '%s' registered but exposes ZERO tools"),
				*Pair.Key);
			++DeadServices;
		}
		TotalTools += Tools.Num();
	}
	UE_LOG(LogTemp, Log,
		TEXT("SpecialAgent: %d services, %d tools total, %d services with zero tools"),
		Services.Num(), TotalTools, DeadServices);
}

FMCPResponse FMCPRequestRouter::RouteRequest(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: RouteRequest called with method: %s"), *Request.Method);
	
	// Validate JSON-RPC version
	if (Request.JsonRpc != TEXT("2.0"))
	{
		return FMCPResponse::Error(Request.Id, -32600, TEXT("Invalid Request: jsonrpc must be '2.0'"));
	}

	// Handle MCP protocol methods
	if (Request.Method == TEXT("initialize"))
	{
		return HandleInitialize(Request);
	}
	
	if (Request.Method == TEXT("tools/list"))
	{
		return HandleToolsList(Request);
	}
	
	if (Request.Method == TEXT("tools/call"))
	{
		return HandleToolsCall(Request, Ctx);
	}

	// Handle server info request
	if (Request.Method == TEXT("server/info") || Request.Method == TEXT("serverInfo"))
	{
		return HandleServerInfo(Request);
	}
	
	// Handle getInstructions - Cursor calls this to get server instructions
	// Match any method containing "instruction" (case-insensitive)
	if (Request.Method.Contains(TEXT("instruction"), ESearchCase::IgnoreCase) ||
	    Request.Method.Contains(TEXT("Instruction"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Matched instruction method: %s"), *Request.Method);
		return HandleGetInstructions(Request);
	}
	
	// Handle resources/list - return available resources
	if (Request.Method == TEXT("resources/list"))
	{
		return HandleResourcesList(Request);
	}
	
	// Handle resources/read - read a specific resource
	if (Request.Method == TEXT("resources/read"))
	{
		return HandleResourcesRead(Request);
	}

	// Handle resources/templates/list - MCP spec method. We expose no
	// parameterized resource templates, so return an empty array (clients
	// like Cursor/Claude Desktop call this during init).
	if (Request.Method == TEXT("resources/templates/list"))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("resourceTemplates"), TArray<TSharedPtr<FJsonValue>>{});
		return FMCPResponse::Success(Request.Id, Result);
	}

	// Handle ping - MCP keep-alive. Spec: empty result.
	if (Request.Method == TEXT("ping"))
	{
		return FMCPResponse::Success(Request.Id, MakeShared<FJsonObject>());
	}

	// Handle prompts/list - return available prompts
	if (Request.Method == TEXT("prompts/list"))
	{
		return HandlePromptsList(Request);
	}
	
	// Handle prompts/get - return a specific prompt
	if (Request.Method == TEXT("prompts/get"))
	{
		return HandlePromptsGet(Request);
	}
	
	// Handle notifications (requests without an ID expecting no response content)
	if (Request.Method == TEXT("notifications/initialized") || Request.Method == TEXT("initialized"))
	{
		// This is a notification - return minimal success response
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		return FMCPResponse::Success(Request.Id, Result);
	}

	// Split method into service prefix and method name
	FString ServicePrefix;
	FString MethodName;
	if (!Request.Method.Split(TEXT("/"), &ServicePrefix, &MethodName))
	{
		return FMCPResponse::Error(
			Request.Id, 
			-32601, 
			TEXT("Method not found: Invalid method format (expected 'service/method')")
		);
	}

	// Find service
	TSharedPtr<IMCPService>* ServicePtr = Services.Find(ServicePrefix);
	if (!ServicePtr || !ServicePtr->IsValid())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("service"), ServicePrefix);
		ErrorData->SetStringField(TEXT("method"), MethodName);

		return FMCPResponse::Error(
			Request.Id,
			-32601,
			FString::Printf(TEXT("Method not found: Service '%s' is not registered"), *ServicePrefix),
			ErrorData
		);
	}

	// Route to service
	TSharedPtr<IMCPService> Service = *ServicePtr;
	return Service->HandleRequest(Request, MethodName, Ctx);
}

void FMCPRequestRouter::RegisterService(const FString& ServicePrefix, TSharedPtr<IMCPService> Service)
{
	Services.Add(ServicePrefix, Service);
	UE_LOG(LogTemp, Verbose, TEXT("SpecialAgent: Registered service '%s'"), *ServicePrefix);
}

FMCPResponse FMCPRequestRouter::HandleInitialize(const FMCPRequest& Request)
{
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: HandleInitialize called, building response..."));

	// Echo back the client's requested protocolVersion — the MCP spec says the
	// server MUST respond with a version it supports, and in practice clients
	// (Claude Code, Cursor, claude-desktop) disconnect if the response version
	// doesn't match their own. Default to 2024-11-05 when no version provided.
	FString RequestedVersion = TEXT("2024-11-05");
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("protocolVersion"), RequestedVersion);
	}
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: initialize protocolVersion=%s"), *RequestedVersion);

	// MCP initialization handshake
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	Result->SetStringField(TEXT("protocolVersion"), RequestedVersion);
	Result->SetStringField(TEXT("instructions"), BuildSpecialAgentInstructions());
	
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("SpecialAgent"));
	ServerInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
	
	// Declare capabilities - tools supported, resources/prompts supported
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	
	// Tools capability
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);
	
	// Resources capability
	TSharedPtr<FJsonObject> ResourcesCap = MakeShared<FJsonObject>();
	ResourcesCap->SetBoolField(TEXT("subscribe"), false);
	ResourcesCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("resources"), ResourcesCap);
	
	// Prompts capability
	TSharedPtr<FJsonObject> PromptsCap = MakeShared<FJsonObject>();
	PromptsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("prompts"), PromptsCap);
	
	Result->SetObjectField(TEXT("capabilities"), Capabilities);
	
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Initialize response ready, sending..."));
	
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FMCPRequestRouter::HandleToolsList(const FMCPRequest& Request)
{
	// Return list of available tools
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	
	// Collect tools from all services
	for (const auto& ServicePair : Services)
	{
		TArray<FMCPToolInfo> ServiceTools = ServicePair.Value->GetAvailableTools();
		
		for (const FMCPToolInfo& ToolInfo : ServiceTools)
		{
			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), FString::Printf(TEXT("%s/%s"), *ServicePair.Key, *ToolInfo.Name));
			ToolObj->SetStringField(TEXT("description"), ToolInfo.Description);
			
			// Add input schema
			TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
			InputSchema->SetObjectField(TEXT("properties"), ToolInfo.Parameters);
			if (ToolInfo.RequiredParams.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> RequiredArray;
				for (const FString& Param : ToolInfo.RequiredParams)
				{
					RequiredArray.Add(MakeShared<FJsonValueString>(Param));
				}
				InputSchema->SetArrayField(TEXT("required"), RequiredArray);
			}
			
			ToolObj->SetObjectField(TEXT("inputSchema"), InputSchema);
			ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
		}
	}
	
	Result->SetArrayField(TEXT("tools"), ToolsArray);
	
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Returning %d tools"), ToolsArray.Num());
	
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FMCPRequestRouter::HandleToolsCall(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	// Execute a tool
	if (!Request.Params.IsValid())
	{
		return FMCPResponse::Error(Request.Id, -32602, TEXT("Invalid params"));
	}
	
	FString ToolName = Request.Params->GetStringField(TEXT("name"));
	TSharedPtr<FJsonObject> Arguments = Request.Params->GetObjectField(TEXT("arguments"));
	
	// Split tool name into service/method
	FString ServicePrefix;
	FString MethodName;
	if (!ToolName.Split(TEXT("/"), &ServicePrefix, &MethodName))
	{
		return FMCPResponse::Error(Request.Id, -32602, TEXT("Invalid tool name format"));
	}
	
	// Find service
	TSharedPtr<IMCPService>* ServicePtr = Services.Find(ServicePrefix);
	if (!ServicePtr || !ServicePtr->IsValid())
	{
		return FMCPResponse::Error(Request.Id, -32601, FString::Printf(TEXT("Service '%s' not found"), *ServicePrefix));
	}
	
	// Create a modified request with the arguments as params
	FMCPRequest ModifiedRequest;
	ModifiedRequest.JsonRpc = Request.JsonRpc;
	ModifiedRequest.Method = ToolName;
	ModifiedRequest.Params = Arguments;
	ModifiedRequest.Id = Request.Id;
	
	// Route to service
	TSharedPtr<IMCPService> Service = *ServicePtr;
	FMCPResponse ServiceResponse = Service->HandleRequest(ModifiedRequest, MethodName, Ctx);

	// Wrap response in MCP content format
	return WrapToolResponse(ServiceResponse, ServicePrefix, MethodName);
}

FMCPResponse FMCPRequestRouter::WrapToolResponse(const FMCPResponse& ServiceResponse, const FString& ServicePrefix, const FString& MethodName)
{
	TSharedPtr<FJsonObject> MCPResult = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArray;
	
	if (ServiceResponse.bSuccess && ServiceResponse.Result.IsValid())
	{
		// Check if this is a screenshot response with base64 data
		FString Base64Data;
		if (ServiceResponse.Result->TryGetStringField(TEXT("base64_data"), Base64Data))
		{
			// Add image content block
			TSharedPtr<FJsonObject> ImageContent = MakeShared<FJsonObject>();
			ImageContent->SetStringField(TEXT("type"), TEXT("image"));
			ImageContent->SetStringField(TEXT("data"), Base64Data);
			ImageContent->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			ContentArray.Add(MakeShared<FJsonValueObject>(ImageContent));
			
			// Also add text description
			TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
			TextContent->SetStringField(TEXT("type"), TEXT("text"));
			
			int32 Width = 0, Height = 0;
			ServiceResponse.Result->TryGetNumberField(TEXT("width"), Width);
			ServiceResponse.Result->TryGetNumberField(TEXT("height"), Height);
			
			TextContent->SetStringField(TEXT("text"), 
				FString::Printf(TEXT("Screenshot captured: %dx%d"), Width, Height));
			ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));
		}
		else
		{
			// Convert result to formatted JSON text
			FString ResultJson;
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = 
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultJson);
			FJsonSerializer::Serialize(ServiceResponse.Result.ToSharedRef(), Writer);
			
			TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
			TextContent->SetStringField(TEXT("type"), TEXT("text"));
			TextContent->SetStringField(TEXT("text"), ResultJson);
			ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));
		}
		
		MCPResult->SetArrayField(TEXT("content"), ContentArray);
		MCPResult->SetBoolField(TEXT("isError"), false);
	}
	else
	{
		// Error response
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		
		FString ErrorMessage = TEXT("Unknown error");
		if (ServiceResponse.ErrorObject.IsValid())
		{
			ServiceResponse.ErrorObject->TryGetStringField(TEXT("message"), ErrorMessage);
		}
		
		TextContent->SetStringField(TEXT("text"), ErrorMessage);
		ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));
		
		MCPResult->SetArrayField(TEXT("content"), ContentArray);
		MCPResult->SetBoolField(TEXT("isError"), true);
	}
	
	return FMCPResponse::Success(ServiceResponse.Id, MCPResult);
}

FMCPResponse FMCPRequestRouter::HandleResourcesList(const FMCPRequest& Request)
{
	// MCP resources/list - return empty list for now (resources may cause client issues)
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Resources;
	Result->SetArrayField(TEXT("resources"), Resources);
	
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Returning empty resources list"));
	
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FMCPRequestRouter::HandleResourcesRead(const FMCPRequest& Request)
{
	// Get the URI from params
	FString Uri;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("uri"), Uri);
	}
	
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: resources/read for URI: %s"), *Uri);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Contents;
	
	if (Uri == TEXT("mcp://instructions") || Uri.Contains(TEXT("instruction"), ESearchCase::IgnoreCase))
	{
		// Return the instructions as text content
		TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("uri"), Uri);
		Content->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Content->SetStringField(TEXT("text"), BuildSpecialAgentInstructions());
		Contents.Add(MakeShared<FJsonValueObject>(Content));
	}
	
	Result->SetArrayField(TEXT("contents"), Contents);
	
	return FMCPResponse::Success(Request.Id, Result);
}

namespace
{
	// Helper: build an argument descriptor for a prompt entry.
	static TSharedPtr<FJsonValue> MakePromptArg(const FString& Name, const FString& Description, bool bRequired)
	{
		TSharedPtr<FJsonObject> Arg = MakeShared<FJsonObject>();
		Arg->SetStringField(TEXT("name"), Name);
		Arg->SetStringField(TEXT("description"), Description);
		Arg->SetBoolField(TEXT("required"), bRequired);
		return MakeShared<FJsonValueObject>(Arg);
	}

	// Helper: build a prompt list entry.
	static TSharedPtr<FJsonValue> MakePromptEntry(const FString& Name, const FString& Description, TArray<TSharedPtr<FJsonValue>> Args)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Name);
		P->SetStringField(TEXT("description"), Description);
		P->SetArrayField(TEXT("arguments"), Args);
		return MakeShared<FJsonValueObject>(P);
	}
}

FMCPResponse FMCPRequestRouter::HandlePromptsList(const FMCPRequest& Request)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Prompts;

	// Existing 4 prompts
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("explore_level"),
			TEXT("Screenshot, list actors, focus interesting ones, summarize."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("search_term"), TEXT("Substring to match actor names against"), true));
		Prompts.Add(MakePromptEntry(TEXT("find_actor"),
			TEXT("Find and focus actors whose names match a search term."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("inspect_selection"),
			TEXT("Inspect currently selected actors (bounds, properties, screenshots)."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("description"), TEXT("Natural language description of what to place"), true));
		Prompts.Add(MakePromptEntry(TEXT("place_objects"),
			TEXT("Place objects in the level using Python + screenshots to verify."), Args));
	}

	// 12 new prompts
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("build_scene"),
			TEXT("Populate a scene with lighting + foliage + streaming, screenshotting between steps."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("parent_class"), TEXT("Parent class path (e.g. /Script/Engine.Actor)"), true));
		Args.Add(MakePromptArg(TEXT("bp_name"), TEXT("Blueprint asset name"), true));
		Args.Add(MakePromptArg(TEXT("asset_path"), TEXT("Content Browser folder path (e.g. /Game/BP)"), true));
		Prompts.Add(MakePromptEntry(TEXT("create_blueprint"),
			TEXT("Create, compile and spawn a new Blueprint, screenshot to verify."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("source_path"), TEXT("Source folder on disk to import from"), true));
		Args.Add(MakePromptArg(TEXT("destination"), TEXT("Target Content Browser path"), true));
		Prompts.Add(MakePromptEntry(TEXT("import_assets"),
			TEXT("Import assets from a folder then run validation on the results."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("duration_seconds"), TEXT("Playback duration in seconds"), true));
		Prompts.Add(MakePromptEntry(TEXT("build_sequence"),
			TEXT("Create a Level Sequence, add transform tracks and keyframes, set playback range."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("time_of_day"), TEXT("Hours, 0-24 (e.g. 6.5 = sunrise)"), true));
		Prompts.Add(MakePromptEntry(TEXT("setup_lighting"),
			TEXT("Spawn sky/fog/clouds/sun for a given time of day and build lighting."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("foliage_type_path"), TEXT("Foliage type asset path"), true));
		Args.Add(MakePromptArg(TEXT("density"), TEXT("Instances per square meter"), true));
		Prompts.Add(MakePromptEntry(TEXT("populate_foliage"),
			TEXT("Paint foliage across the level bounds at a given density, screenshot."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("size"), TEXT("Landscape size in components (e.g. 8)"), true));
		Args.Add(MakePromptArg(TEXT("layers"), TEXT("Comma-separated list of material layer names"), true));
		Prompts.Add(MakePromptEntry(TEXT("build_landscape"),
			TEXT("Create a landscape, sculpt regions, and paint material layers."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("configure_postprocess"),
			TEXT("Spawn a post-process volume and tune exposure, bloom and DoF."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("setup_navigation"),
			TEXT("Add a nav bounds volume, rebuild the navmesh, test a path."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("wire_gameplay"),
			TEXT("Spawn player start, killz volume, trigger volume, and splined markers."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Prompts.Add(MakePromptEntry(TEXT("debug_performance"),
			TEXT("Collect stats, overlaps and triangle counts and report hot spots."), Args));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakePromptArg(TEXT("duration_seconds"), TEXT("Seconds to keep PIE running"), true));
		Prompts.Add(MakePromptEntry(TEXT("run_pie_test"),
			TEXT("Start PIE, wait, tail the log, stop PIE, and summarize."), Args));
	}

	Result->SetArrayField(TEXT("prompts"), Prompts);

	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Returning %d prompts"), Prompts.Num());

	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FMCPRequestRouter::HandlePromptsGet(const FMCPRequest& Request)
{
	FString PromptName;
	TSharedPtr<FJsonObject> Arguments;
	
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("name"), PromptName);
		const TSharedPtr<FJsonObject>* ArgsObj;
		if (Request.Params->TryGetObjectField(TEXT("arguments"), ArgsObj))
		{
			Arguments = *ArgsObj;
		}
	}
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Messages;
	
	if (PromptName == TEXT("explore_level"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Please explore the current Unreal Engine level:\n"
			"1. First, take a screenshot to see the current viewport view\n"
			"2. List all actors in the level to understand what exists\n"
			"3. Focus on interesting actors and take screenshots of them\n"
			"4. Summarize what you found in the level"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("find_actor"))
	{
		FString SearchTerm;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("search_term"), SearchTerm);
		}
		
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Find and focus on actors matching '%s':\n"
			"1. List actors and filter for ones matching the search term\n"
			"2. Use viewport/focus_actor to frame each matching actor\n"
			"3. Take a screenshot after focusing to show me the actor\n"
			"4. Report what you found with key details (location, bounds, etc.)"
		), *SearchTerm));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("inspect_selection"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Inspect the currently selected actors:\n"
			"1. Use utility/get_selection to see what's selected\n"
			"2. Use utility/get_selection_bounds to get detailed bounds and orientation\n"
			"3. Focus on each selected actor and take a screenshot\n"
			"4. Summarize the selection with key properties"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("place_objects"))
	{
		FString Description;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("description"), Description);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Help me place objects in the level: %s\n\n"
			"Use Python (python/execute) with the unreal module to:\n"
			"1. First screenshot to see the current state\n"
			"2. Use unreal.EditorLevelLibrary or unreal.EditorAssetLibrary as needed\n"
			"3. Place/modify the requested objects\n"
			"4. Screenshot again to verify the results"
		), *Description));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("build_scene"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Populate the current level with a basic playable scene:\n"
			"1. screenshot/capture to record starting state\n"
			"2. world/place_in_grid to distribute static meshes across an area\n"
			"3. lighting/spawn_light to add a directional + a few point lights; then lighting/build_lighting\n"
			"4. foliage/paint_in_area to cover the ground\n"
			"5. streaming/load_level (or streaming/create_streaming_level) to bring in any sub-levels\n"
			"6. screenshot/capture between major steps and at the end to verify"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("create_blueprint"))
	{
		FString ParentClass;
		FString BpName;
		FString AssetPath;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("parent_class"), ParentClass);
			Arguments->TryGetStringField(TEXT("bp_name"), BpName);
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Create a new Blueprint and verify it spawns:\n"
			"1. blueprint/create with parent_class='%s', name='%s', path='%s'\n"
			"2. blueprint/compile the new asset\n"
			"3. world/spawn_actor to place an instance in the level\n"
			"4. screenshot/capture to verify the spawn"
		), *ParentClass, *BpName, *AssetPath));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("import_assets"))
	{
		FString SourcePath;
		FString Destination;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("source_path"), SourcePath);
			Arguments->TryGetStringField(TEXT("destination"), Destination);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Import assets from disk and validate them:\n"
			"1. asset_import/import_folder with source='%s' destination='%s'\n"
			"2. validation/validate_selected on the freshly imported assets\n"
			"3. Report any validation errors or warnings"
		), *SourcePath, *Destination));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("build_sequence"))
	{
		FString DurationSeconds;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("duration_seconds"), DurationSeconds);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Build a simple Level Sequence %s seconds long:\n"
			"1. sequencer/create to make a new Level Sequence asset\n"
			"2. sequencer/add_actor_binding for each actor you want animated\n"
			"3. sequencer/add_transform_track + sequencer/add_keyframe for start and end poses\n"
			"4. sequencer/set_playback_range to cover the full duration\n"
			"5. screenshot/capture the Sequencer editor to verify"
		), *DurationSeconds));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("setup_lighting"))
	{
		FString TimeOfDay;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("time_of_day"), TimeOfDay);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Set up natural outdoor lighting for time_of_day=%s (hours 0-24):\n"
			"1. sky/spawn_sky_atmosphere\n"
			"2. sky/spawn_height_fog\n"
			"3. sky/spawn_volumetric_cloud\n"
			"4. sky/spawn_sky_light\n"
			"5. lighting/spawn_light (directional) positioned to represent the sun for that time\n"
			"6. lighting/build_lighting\n"
			"7. screenshot/capture to verify the mood"
		), *TimeOfDay));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("populate_foliage"))
	{
		FString FoliageTypePath;
		FString Density;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("foliage_type_path"), FoliageTypePath);
			Arguments->TryGetStringField(TEXT("density"), Density);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Populate foliage across the full level bounds:\n"
			"1. foliage/paint_in_area using foliage_type='%s' and density=%s\n"
			"2. Cover the level extents (query world bounds if needed)\n"
			"3. screenshot/capture to verify coverage"
		), *FoliageTypePath, *Density));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("build_landscape"))
	{
		FString Size;
		FString Layers;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("size"), Size);
			Arguments->TryGetStringField(TEXT("layers"), Layers);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Build a landscape of size=%s with layers=[%s]:\n"
			"1. python/execute to create the landscape actor (UE editor API fallback)\n"
			"2. landscape/sculpt to shape a few regions\n"
			"3. landscape/paint_layer for each layer in the list\n"
			"4. screenshot/capture to verify"
		), *Size, *Layers));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("configure_postprocess"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Spawn and tune a post-process volume:\n"
			"1. post_process/spawn_volume (unbound)\n"
			"2. post_process/set_exposure to a reasonable min/max\n"
			"3. post_process/set_bloom to tasteful intensity\n"
			"4. post_process/set_dof for focal distance/aperture\n"
			"5. screenshot/capture before and after to show the effect"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("setup_navigation"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Set up navigation for the current level:\n"
			"1. python/execute to spawn a NavMeshBoundsVolume covering the playable area\n"
			"2. navigation/rebuild_navmesh\n"
			"3. navigation/test_path between two sample points to confirm it works\n"
			"4. screenshot/capture in nav view mode to verify coverage"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("wire_gameplay"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Wire the essential gameplay actors into this level:\n"
			"1. gameplay/spawn_player_start where the player should appear\n"
			"2. gameplay/spawn_killz_volume below the playable area\n"
			"3. gameplay/spawn_trigger_volume near the objective\n"
			"4. gameplay/place_along_spline to mark the path toward the goal\n"
			"5. screenshot/capture to verify placement"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("debug_performance"))
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), TEXT(
			"Diagnose level performance:\n"
			"1. performance/get_statistics for overall stats\n"
			"2. performance/check_overlaps to find geometric collisions\n"
			"3. performance/get_triangle_count to find heavy meshes\n"
			"4. Summarize the top hot spots and recommend fixes"
		));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else if (PromptName == TEXT("run_pie_test"))
	{
		FString DurationSeconds;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("duration_seconds"), DurationSeconds);
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT(
			"Run a PIE smoke test for %s seconds:\n"
			"1. pie/start\n"
			"2. Wait %s seconds (sleep via python/execute if needed)\n"
			"3. log/tail to collect recent log output\n"
			"4. pie/stop\n"
			"5. Summarize any errors or warnings found in the tail"
		), *DurationSeconds, *DurationSeconds));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}
	else
	{
		return FMCPResponse::Error(Request.Id, -32602, FString::Printf(TEXT("Unknown prompt: %s"), *PromptName));
	}
	
	Result->SetStringField(TEXT("description"), FString::Printf(TEXT("Prompt: %s"), *PromptName));
	Result->SetArrayField(TEXT("messages"), Messages);
	
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FMCPRequestRouter::HandleServerInfo(const FMCPRequest& Request)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	
	Result->SetStringField(TEXT("name"), TEXT("SpecialAgent"));
	Result->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Result->SetStringField(TEXT("protocol_version"), TEXT("2.0"));
	Result->SetStringField(TEXT("description"), TEXT("MCP Server for Unreal Engine 5"));
	Result->SetStringField(TEXT("instructions"), BuildSpecialAgentInstructions());
	
	// List available services
	TArray<TSharedPtr<FJsonValue>> ServiceArray;
	for (const auto& ServicePair : Services)
	{
		TSharedPtr<FJsonObject> ServiceObj = MakeShared<FJsonObject>();
		ServiceObj->SetStringField(TEXT("prefix"), ServicePair.Key);
		ServiceObj->SetStringField(TEXT("description"), ServicePair.Value->GetServiceDescription());
		
		ServiceArray.Add(MakeShared<FJsonValueObject>(ServiceObj));
	}
	Result->SetArrayField(TEXT("services"), ServiceArray);

	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FMCPRequestRouter::HandleGetInstructions(const FMCPRequest& Request)
{
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Handling getInstructions request"));
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("instructions"), BuildSpecialAgentInstructions());
	
	return FMCPResponse::Success(Request.Id, Result);
}


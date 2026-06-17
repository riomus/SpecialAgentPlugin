// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/StreamingService.h"
#include "MCPCommon/MCPRequestContext.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/LevelStreamingVolume.h"
#include "Engine/World.h"

FStreamingService::FStreamingService()
{
}

FString FStreamingService::GetServiceDescription() const
{
	return TEXT("Level streaming management - list, load, unload, and control visibility of sublevels and streaming volumes");
}

FMCPResponse FStreamingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("list_levels")) return HandleListLevels(Request);
	if (MethodName == TEXT("load_level")) return HandleLoadLevel(Request);
	if (MethodName == TEXT("unload_level")) return HandleUnloadLevel(Request);
	if (MethodName == TEXT("set_level_visibility")) return HandleSetLevelVisibility(Request);
	if (MethodName == TEXT("set_level_streaming_volume")) return HandleSetLevelStreamingVolume(Request);

	return MethodNotFound(Request.Id, TEXT("streaming"), MethodName);
}

// Utility: locate a streaming level whose package name matches the input.
// Accepts either a short package name (e.g. "MyLevel") or a full path (/Game/Maps/MyLevel).
static ULevelStreaming* FindStreamingLevelByName(UWorld* World, const FString& LevelName)
{
	if (!World) return nullptr;

	const FString Target = LevelName;
	const FName TargetShort(*FPackageName::GetShortName(LevelName));

	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (!Streaming) continue;

		const FString PackageName = Streaming->GetWorldAssetPackageName();
		if (PackageName == Target)
		{
			return Streaming;
		}

		const FName ShortName(*FPackageName::GetShortName(PackageName));
		if (ShortName == TargetShort)
		{
			return Streaming;
		}
	}
	return nullptr;
}

FMCPResponse FStreamingService::HandleListLevels(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();

		TArray<TSharedPtr<FJsonValue>> LevelsJson;

		const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
		for (ULevelStreaming* Streaming : StreamingLevels)
		{
			if (!Streaming) continue;

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("package_name"), Streaming->GetWorldAssetPackageName());
			Entry->SetStringField(TEXT("class"), Streaming->GetClass()->GetName());
			Entry->SetBoolField(TEXT("is_loaded"), Streaming->IsLevelLoaded());
			Entry->SetBoolField(TEXT("is_visible"), Streaming->IsLevelVisible());
			Entry->SetBoolField(TEXT("should_be_loaded"), Streaming->ShouldBeLoaded());
			Entry->SetBoolField(TEXT("should_be_visible"), Streaming->GetShouldBeVisibleFlag());
			Entry->SetNumberField(TEXT("lod_index"), Streaming->GetLevelLODIndex());

			LevelsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}

		// Include the persistent level so the LLM knows the base map.
		if (ULevel* Persistent = World->PersistentLevel)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			const FString PersistentPackage = Persistent->GetOutermost() ? Persistent->GetOutermost()->GetName() : TEXT("");
			Entry->SetStringField(TEXT("package_name"), PersistentPackage);
			Entry->SetStringField(TEXT("class"), TEXT("PersistentLevel"));
			Entry->SetBoolField(TEXT("is_loaded"), true);
			Entry->SetBoolField(TEXT("is_visible"), true);
			Entry->SetBoolField(TEXT("is_persistent"), true);
			LevelsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Result->SetArrayField(TEXT("levels"), LevelsJson);
		Result->SetNumberField(TEXT("count"), LevelsJson.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: streaming/list_levels returned %d entries"), LevelsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FStreamingService::HandleLoadLevel(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString LevelName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("level_name"), LevelName) || LevelName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'level_name' (string, package path or short name)"));
	}

	FVector Location = FVector::ZeroVector;
	FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);

	FRotator Rotation = FRotator::ZeroRotator;
	FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

	bool bMakeVisible = true;
	FMCPJson::ReadBool(Request.Params, TEXT("make_visible"), bMakeVisible);

	auto Task = [LevelName, Location, Rotation, bMakeVisible]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		// First try: if the level is already a registered streaming level, simply flag it.
		if (ULevelStreaming* Existing = FindStreamingLevelByName(World, LevelName))
		{
			Existing->SetShouldBeLoaded(true);
			Existing->SetShouldBeVisible(bMakeVisible);
			World->FlushLevelStreaming(EFlushLevelStreamingType::Full);

			TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
			Result->SetStringField(TEXT("package_name"), Existing->GetWorldAssetPackageName());
			Result->SetBoolField(TEXT("is_loaded"), Existing->IsLevelLoaded());
			Result->SetBoolField(TEXT("is_visible"), Existing->IsLevelVisible());
			Result->SetStringField(TEXT("path"), TEXT("existing_streaming_level"));

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: streaming/load_level loaded existing level '%s'"), *LevelName);
			return Result;
		}

		// Otherwise, spawn a new dynamic streaming instance for the level asset.
		bool bOutSuccess = false;
		ULevelStreamingDynamic* Created = ULevelStreamingDynamic::LoadLevelInstance(
			World, LevelName, Location, Rotation, bOutSuccess);

		if (!bOutSuccess || !Created)
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Failed to load level instance '%s' (asset missing, already loaded, or invalid)"), *LevelName));
		}

		Created->SetShouldBeVisible(bMakeVisible);
		World->FlushLevelStreaming(EFlushLevelStreamingType::Full);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("package_name"), Created->GetWorldAssetPackageName());
		Result->SetBoolField(TEXT("is_loaded"), Created->IsLevelLoaded());
		Result->SetBoolField(TEXT("is_visible"), Created->IsLevelVisible());
		Result->SetStringField(TEXT("path"), TEXT("dynamic_instance"));

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: streaming/load_level spawned dynamic instance for '%s'"), *LevelName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FStreamingService::HandleUnloadLevel(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString LevelName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("level_name"), LevelName) || LevelName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'level_name'"));
	}

	auto Task = [LevelName]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		ULevelStreaming* Streaming = FindStreamingLevelByName(World, LevelName);
		if (!Streaming)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Streaming level '%s' not found"), *LevelName));
		}

		Streaming->SetShouldBeVisible(false);
		Streaming->SetShouldBeLoaded(false);
		Streaming->SetIsRequestingUnloadAndRemoval(true);
		World->FlushLevelStreaming(EFlushLevelStreamingType::Full);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("package_name"), Streaming->GetWorldAssetPackageName());
		Result->SetBoolField(TEXT("is_loaded"), Streaming->IsLevelLoaded());
		Result->SetBoolField(TEXT("is_visible"), Streaming->IsLevelVisible());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: streaming/unload_level unloaded '%s'"), *LevelName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FStreamingService::HandleSetLevelVisibility(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString LevelName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("level_name"), LevelName) || LevelName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'level_name'"));
	}

	bool bVisible = true;
	if (!FMCPJson::ReadBool(Request.Params, TEXT("visible"), bVisible))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'visible' (bool)"));
	}

	auto Task = [LevelName, bVisible]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		ULevelStreaming* Streaming = FindStreamingLevelByName(World, LevelName);
		if (!Streaming)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Streaming level '%s' not found"), *LevelName));
		}

		Streaming->SetShouldBeVisible(bVisible);
		World->FlushLevelStreaming(EFlushLevelStreamingType::Visibility);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("package_name"), Streaming->GetWorldAssetPackageName());
		Result->SetBoolField(TEXT("should_be_visible"), Streaming->GetShouldBeVisibleFlag());
		Result->SetBoolField(TEXT("is_visible"), Streaming->IsLevelVisible());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: streaming/set_level_visibility '%s' -> %s"),
			*LevelName, bVisible ? TEXT("visible") : TEXT("hidden"));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FStreamingService::HandleSetLevelStreamingVolume(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString LevelName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("level_name"), LevelName) || LevelName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'level_name'"));
	}

	FVector Location = FVector::ZeroVector;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'location' ([X,Y,Z] world cm)"));
	}

	FVector Extent(1000.0, 1000.0, 1000.0);
	FMCPJson::ReadVec3(Request.Params, TEXT("extent"), Extent);

	FString UsageStr(TEXT("loading_and_visibility"));
	FMCPJson::ReadString(Request.Params, TEXT("usage"), UsageStr);

	auto Task = [LevelName, Location, Extent, UsageStr]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		ULevelStreaming* Streaming = FindStreamingLevelByName(World, LevelName);
		if (!Streaming)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Streaming level '%s' not found"), *LevelName));
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ALevelStreamingVolume* Volume = World->SpawnActor<ALevelStreamingVolume>(
			ALevelStreamingVolume::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
		if (!Volume)
		{
			return FMCPJson::MakeError(TEXT("Failed to spawn ALevelStreamingVolume"));
		}

		// Scale the default unit cube brush by the requested extent.
		Volume->SetActorScale3D(Extent / 100.0);

		// Bind the volume to the streaming level.
		const FName PackageShort(*FPackageName::GetShortName(Streaming->GetWorldAssetPackageName()));
		Volume->StreamingLevelNames.AddUnique(PackageShort);

		// Usage translation.
		EStreamingVolumeUsage Usage = SVB_LoadingAndVisibility;
		if (UsageStr.Equals(TEXT("loading"), ESearchCase::IgnoreCase)) Usage = SVB_Loading;
		else if (UsageStr.Equals(TEXT("visibility_blocking_on_load"), ESearchCase::IgnoreCase)) Usage = SVB_VisibilityBlockingOnLoad;
		else if (UsageStr.Equals(TEXT("blocking_on_load"), ESearchCase::IgnoreCase)) Usage = SVB_BlockingOnLoad;
		else if (UsageStr.Equals(TEXT("loading_not_visible"), ESearchCase::IgnoreCase)) Usage = SVB_LoadingNotVisible;
		Volume->StreamingUsage = Usage;

		// Attach the volume to the level's EditorStreamingVolumes array so the engine picks it up.
		Streaming->EditorStreamingVolumes.AddUnique(Volume);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("package_name"), Streaming->GetWorldAssetPackageName());
		Result->SetStringField(TEXT("volume_label"), Volume->GetActorLabel());
		Result->SetStringField(TEXT("usage"), UsageStr);
		FMCPJson::WriteVec3(Result, TEXT("location"), Location);
		FMCPJson::WriteVec3(Result, TEXT("extent"), Extent);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: streaming/set_level_streaming_volume bound volume '%s' to level '%s'"),
			*Volume->GetActorLabel(), *LevelName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FStreamingService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
		TEXT("list_levels"),
		TEXT("List the streaming sublevels (ULevelStreaming) registered on the current editor world, plus the persistent base level. "
		     "Returns {levels:[{package_name (/Game/... path), class, is_loaded, is_visible, should_be_loaded, should_be_visible, lod_index}], count}; "
		     "the persistent entry adds is_persistent=true. "
		     "Params: (none). "
		     "Workflow: Read-only; call first to get a package_name to feed load_level / unload_level / set_level_visibility. "
		     "Warning: World Partition cells are not ULevelStreaming sublevels; use world_partition/list_cells for those."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("load_level"),
		TEXT("Load a streaming sublevel into the editor world. If level_name already matches a registered ULevelStreaming it just flags it loaded/visible; otherwise it spawns a fresh ULevelStreamingDynamic instance at location/rotation. "
		     "Returns {package_name, is_loaded, is_visible, path:'existing_streaming_level'|'dynamic_instance'}. "
		     "Params: level_name (string, /Game/Maps/MyLevel path or short package name, required), location ([X,Y,Z] world-space cm, optional, default [0,0,0], applied only to new dynamic instances), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0], new instances only), make_visible (bool, optional, default true). "
		     "Workflow: Call list_levels first to discover sublevels and their package_name. "
		     "Warning: Mutates in-memory streaming state and runs a blocking FlushLevelStreaming (Full) on the game thread; not persisted to disk. Fails if the level asset is missing or already loaded as a dynamic instance."))
		.RequiredString(TEXT("level_name"), TEXT("Level package path (/Game/Maps/MyLevel) or short name"))
		.OptionalVec3(TEXT("location"), TEXT("Spawn location for new dynamic instances [X,Y,Z] world cm; default [0,0,0]"))
		.OptionalVec3(TEXT("rotation"), TEXT("Spawn rotation for new dynamic instances [Pitch,Yaw,Roll] degrees; default [0,0,0]"))
		.OptionalBool(TEXT("make_visible"), TEXT("Set should-be-visible after loading; default true"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("unload_level"),
		TEXT("Unload a registered streaming sublevel: clears should-be-loaded and should-be-visible, requests removal, then flushes streaming. "
		     "Returns {package_name, is_loaded, is_visible}. "
		     "Params: level_name (string, package path or short name, required; must match a ULevelStreaming from list_levels). "
		     "Workflow: Call list_levels first to confirm the level is loaded and get the exact package_name. "
		     "Warning: Mutates in-memory state via a blocking FlushLevelStreaming (Full); not persisted. The persistent base level cannot be unloaded, and unknown names return an error."))
		.RequiredString(TEXT("level_name"), TEXT("Level package path or short name"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("set_level_visibility"),
		TEXT("Show or hide an already-loaded streaming sublevel without unloading it, by toggling ULevelStreaming should-be-visible and flushing visibility. "
		     "Returns {package_name, should_be_visible, is_visible}. "
		     "Params: level_name (string, package path or short name, required), visible (bool, required; true shows, false hides). "
		     "Workflow: Call list_levels to confirm is_loaded; cheaper than unload_level/load_level for flicker-free toggling. "
		     "Warning: Changes in-memory state only (not saved); runs a blocking visibility flush. Unknown names return an error."))
		.RequiredString(TEXT("level_name"), TEXT("Level package path or short name"))
		.RequiredBool(TEXT("visible"), TEXT("true to show, false to hide"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("set_level_streaming_volume"),
		TEXT("Spawn an ALevelStreamingVolume centered at location, sized to extent, and bind it to a streaming sublevel so the volume drives that level's load/visibility. "
		     "Returns {package_name, volume_label, usage, location, extent}. "
		     "Params: level_name (string, required; must match a ULevelStreaming from list_levels), location ([X,Y,Z] world-space cm, required, volume center), extent ([X,Y,Z] cm full box size, optional, default [1000,1000,1000]; applied as actor scale of the unit-cube brush), usage (enum string, optional, default 'loading_and_visibility'). "
		     "Workflow: Call list_levels to pick the level, then place the volume so streaming follows the player/camera position. "
		     "Warning: Spawns a new actor in the level (in-memory only, not saved) and the streaming volume only activates when the camera is inside it; for an editor-time preview load the sublevel directly with load_level. Unknown level names return an error."))
		.RequiredString(TEXT("level_name"), TEXT("Level package path or short name"))
		.RequiredVec3(TEXT("location"), TEXT("Volume center [X,Y,Z] world cm"))
		.OptionalVec3(TEXT("extent"), TEXT("Volume full box size [X,Y,Z] cm; default [1000,1000,1000]"))
		.OptionalEnum(TEXT("usage"), {
			TEXT("loading"),
			TEXT("loading_and_visibility"),
			TEXT("visibility_blocking_on_load"),
			TEXT("blocking_on_load"),
			TEXT("loading_not_visible")
		}, TEXT("Streaming usage mode; default loading_and_visibility"))
		.Build());

	return Tools;
}

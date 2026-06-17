// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ViewportService.h"
#include "MCPCommon/MCPRequestContext.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPViewport.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "SceneView.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Selection.h"
#include "ShowFlags.h"
#include "Bookmarks/IBookmarkTypeTools.h"
#include "Settings/LevelEditorViewportSettings.h"

FViewportService::FViewportService()
{
}

FString FViewportService::GetServiceDescription() const
{
	return TEXT("Viewport camera control - position camera for screenshot capture");
}

FMCPResponse FViewportService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("set_location")) return HandleSetLocation(Request);
	if (MethodName == TEXT("set_rotation")) return HandleSetRotation(Request);
	if (MethodName == TEXT("get_transform")) return HandleGetTransform(Request);
	if (MethodName == TEXT("focus_actor")) return HandleFocusActor(Request);
	if (MethodName == TEXT("trace_from_screen")) return HandleTraceFromScreen(Request);

	// Phase 1.A
	if (MethodName == TEXT("orbit_around_actor")) return HandleOrbitAroundActor(Request);
	if (MethodName == TEXT("set_fov")) return HandleSetFov(Request);
	if (MethodName == TEXT("set_view_mode")) return HandleSetViewMode(Request);
	if (MethodName == TEXT("toggle_game_view")) return HandleToggleGameView(Request);
	if (MethodName == TEXT("bookmark_save")) return HandleBookmarkSave(Request);
	if (MethodName == TEXT("bookmark_restore")) return HandleBookmarkRestore(Request);
	if (MethodName == TEXT("set_grid_snap")) return HandleSetGridSnap(Request);
	if (MethodName == TEXT("toggle_realtime")) return HandleToggleRealtime(Request);
	if (MethodName == TEXT("force_redraw")) return HandleForceRedraw(Request);

	return MethodNotFound(Request.Id, TEXT("viewport"), MethodName);
}

FMCPResponse FViewportService::HandleSetLocation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	// Get location array
	const TArray<TSharedPtr<FJsonValue>>* LocationArray;
	if (!Request.Params->TryGetArrayField(TEXT("location"), LocationArray) || LocationArray->Num() != 3)
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' parameter (expected array of 3 numbers)"));
	}

	FVector Location(
		(*LocationArray)[0]->AsNumber(),
		(*LocationArray)[1]->AsNumber(),
		(*LocationArray)[2]->AsNumber()
	);

	auto SetLocationTask = [Location]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = FMCPViewport::GetLevelClient();
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No Level Editor viewport available"));
			return Result;
		}

		ViewportClient->SetViewLocation(Location);
		ViewportClient->Invalidate();
		
		Result->SetBoolField(TEXT("success"), true);
		TArray<TSharedPtr<FJsonValue>> LocationJson;
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Z));
		Result->SetArrayField(TEXT("location"), LocationJson);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Viewport location set to: (%.1f, %.1f, %.1f)"), 
			Location.X, Location.Y, Location.Z);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SetLocationTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleSetRotation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	// Get rotation array
	const TArray<TSharedPtr<FJsonValue>>* RotationArray;
	if (!Request.Params->TryGetArrayField(TEXT("rotation"), RotationArray) || RotationArray->Num() != 3)
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'rotation' parameter (expected array of 3 numbers)"));
	}

	FRotator Rotation(
		(*RotationArray)[0]->AsNumber(),
		(*RotationArray)[1]->AsNumber(),
		(*RotationArray)[2]->AsNumber()
	);

	auto SetRotationTask = [Rotation]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = FMCPViewport::GetLevelClient();
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No Level Editor viewport available"));
			return Result;
		}

		ViewportClient->SetViewRotation(Rotation);
		ViewportClient->Invalidate();
		
		Result->SetBoolField(TEXT("success"), true);
		TArray<TSharedPtr<FJsonValue>> RotationJson;
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
		Result->SetArrayField(TEXT("rotation"), RotationJson);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Viewport rotation set to: (%.1f, %.1f, %.1f)"), 
			Rotation.Pitch, Rotation.Yaw, Rotation.Roll);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SetRotationTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleGetTransform(const FMCPRequest& Request)
{
	auto GetTransformTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = FMCPViewport::GetLevelClient();
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No Level Editor viewport available"));
			return Result;
		}

		FVector Location = ViewportClient->GetViewLocation();
		FRotator Rotation = ViewportClient->GetViewRotation();

		TArray<TSharedPtr<FJsonValue>> LocationJson;
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Z));

		TArray<TSharedPtr<FJsonValue>> RotationJson;
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("location"), LocationJson);
		Result->SetArrayField(TEXT("rotation"), RotationJson);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetTransformTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleFocusActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'actor_name'"));
	}

	auto FocusTask = [ActorName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Find actor by label first, then by internal name (ID)
		AActor* FoundActor = nullptr;
		FString MatchedBy;
		
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			
			// First priority: exact label match
			if (Actor->GetActorLabel() == ActorName)
			{
				FoundActor = Actor;
				MatchedBy = TEXT("label");
				break;
			}
			// Second priority: exact internal name match (like pressing F in editor)
			if (Actor->GetName() == ActorName)
			{
				FoundActor = Actor;
				MatchedBy = TEXT("name");
				break;
			}
		}
		
		// If no exact match, try case-insensitive partial match on label
		if (!FoundActor)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) continue;
				
				if (Actor->GetActorLabel().Contains(ActorName, ESearchCase::IgnoreCase) ||
				    Actor->GetName().Contains(ActorName, ESearchCase::IgnoreCase))
				{
					FoundActor = Actor;
					MatchedBy = TEXT("partial");
					break;
				}
			}
		}

		if (!FoundActor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
			return Result;
		}

		// Focus on the actor (like pressing F in editor)
		FLevelEditorViewportClient* ViewportClient = FMCPViewport::GetLevelClient();
		if (ViewportClient)
		{
			ViewportClient->FocusViewportOnBox(FoundActor->GetComponentsBoundingBox());
			ViewportClient->Invalidate();
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("actor_name"), FoundActor->GetActorLabel());
		Result->SetStringField(TEXT("actor_id"), FoundActor->GetName());
		Result->SetStringField(TEXT("matched_by"), MatchedBy);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Focused viewport on actor: %s (ID: %s, matched by: %s)"), 
			*FoundActor->GetActorLabel(), *FoundActor->GetName(), *MatchedBy);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(FocusTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleTraceFromScreen(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	// Get screen position as percentage (0.0 to 1.0)
	double ScreenX = 0.5;  // Default to center
	double ScreenY = 0.5;
	
	Request.Params->TryGetNumberField(TEXT("screen_x"), ScreenX);
	Request.Params->TryGetNumberField(TEXT("screen_y"), ScreenY);
	
	// Clamp to valid range
	ScreenX = FMath::Clamp(ScreenX, 0.0, 1.0);
	ScreenY = FMath::Clamp(ScreenY, 0.0, 1.0);

	auto TraceTask = [ScreenX, ScreenY]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = FMCPViewport::GetLevelClient();
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No Level Editor viewport available"));
			return Result;
		}

		FViewport* Viewport = ViewportClient->Viewport;
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Level Editor viewport has no render target"));
			return Result;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Get viewport size and calculate pixel position
		FIntPoint ViewportSize = Viewport->GetSizeXY();
		int32 PixelX = FMath::RoundToInt(ScreenX * ViewportSize.X);
		int32 PixelY = FMath::RoundToInt(ScreenY * ViewportSize.Y);

		// Get ray from screen position
		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			Viewport,
			ViewportClient->GetScene(),
			ViewportClient->EngineShowFlags)
			.SetRealtimeUpdate(true));
		
		FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to calculate scene view"));
			return Result;
		}

		// Deproject screen position to world ray
		FVector WorldOrigin, WorldDirection;
		SceneView->DeprojectFVector2D(FVector2D(PixelX, PixelY), WorldOrigin, WorldDirection);

		// Perform line trace
		FHitResult HitResult;
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(ScreenTrace), true);
		TraceParams.bReturnPhysicalMaterial = true;
		
		float TraceDistance = 100000.0f; // 1km trace distance
		FVector TraceEnd = WorldOrigin + WorldDirection * TraceDistance;
		
		bool bHit = World->LineTraceSingleByChannel(
			HitResult,
			WorldOrigin,
			TraceEnd,
			ECC_Visibility,
			TraceParams
		);

		// Store input parameters
		Result->SetNumberField(TEXT("screen_x"), ScreenX);
		Result->SetNumberField(TEXT("screen_y"), ScreenY);
		Result->SetNumberField(TEXT("pixel_x"), PixelX);
		Result->SetNumberField(TEXT("pixel_y"), PixelY);
		Result->SetNumberField(TEXT("viewport_width"), ViewportSize.X);
		Result->SetNumberField(TEXT("viewport_height"), ViewportSize.Y);

		if (bHit)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), true);
			
			// Hit location
			TArray<TSharedPtr<FJsonValue>> LocationArr;
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.X));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Y));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Z));
			Result->SetArrayField(TEXT("location"), LocationArr);
			
			// Impact normal (surface normal at hit point)
			TArray<TSharedPtr<FJsonValue>> NormalArr;
			NormalArr.Add(MakeShared<FJsonValueNumber>(HitResult.ImpactNormal.X));
			NormalArr.Add(MakeShared<FJsonValueNumber>(HitResult.ImpactNormal.Y));
			NormalArr.Add(MakeShared<FJsonValueNumber>(HitResult.ImpactNormal.Z));
			Result->SetArrayField(TEXT("normal"), NormalArr);
			
			// Distance from camera
			Result->SetNumberField(TEXT("distance"), HitResult.Distance);
			
			// Hit actor info
			AActor* HitActor = HitResult.GetActor();
			if (HitActor)
			{
				Result->SetStringField(TEXT("actor_name"), HitActor->GetActorLabel());
				Result->SetStringField(TEXT("actor_id"), HitActor->GetName());
				Result->SetStringField(TEXT("actor_class"), HitActor->GetClass()->GetName());
			}
			
			// Hit component info
			UPrimitiveComponent* HitComponent = HitResult.GetComponent();
			if (HitComponent)
			{
				Result->SetStringField(TEXT("component_name"), HitComponent->GetName());
			}
			
			// Physical material if available
			if (HitResult.PhysMaterial.IsValid())
			{
				Result->SetStringField(TEXT("physical_material"), HitResult.PhysMaterial->GetName());
			}

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screen trace hit at (%.1f, %.1f) -> Location: (%.1f, %.1f, %.1f), Normal: (%.2f, %.2f, %.2f)"),
				ScreenX, ScreenY, HitResult.Location.X, HitResult.Location.Y, HitResult.Location.Z,
				HitResult.ImpactNormal.X, HitResult.ImpactNormal.Y, HitResult.ImpactNormal.Z);
		}
		else
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), false);
			Result->SetStringField(TEXT("message"), TEXT("No hit - ray did not intersect any geometry"));
			
			// Still return the ray direction for reference
			TArray<TSharedPtr<FJsonValue>> DirArr;
			DirArr.Add(MakeShared<FJsonValueNumber>(WorldDirection.X));
			DirArr.Add(MakeShared<FJsonValueNumber>(WorldDirection.Y));
			DirArr.Add(MakeShared<FJsonValueNumber>(WorldDirection.Z));
			Result->SetArrayField(TEXT("ray_direction"), DirArr);

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screen trace at (%.1f, %.1f) - no hit"), ScreenX, ScreenY);
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(TraceTask);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Phase 1.A additions
// ============================================================================
// Helper: resolve a FLevelEditorViewportClient, or null. Delegates to the
// shared resolver, which never blind-casts a focused non-level viewport client.
static FLevelEditorViewportClient* ResolveLevelViewportClient()
{
	return FMCPViewport::GetLevelClient();
}

FMCPResponse FViewportService::HandleOrbitAroundActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	double Angle = 0.0, Distance = 500.0, Height = 200.0;
	Request.Params->TryGetNumberField(TEXT("angle"), Angle);
	Request.Params->TryGetNumberField(TEXT("distance"), Distance);
	Request.Params->TryGetNumberField(TEXT("height"), Height);

	auto Task = [ActorName, Angle, Distance, Height]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));

		FBox Bounds = Actor->GetComponentsBoundingBox(true);
		FVector Center = Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
		const double Rad = FMath::DegreesToRadians(Angle);
		const FVector Offset(FMath::Cos(Rad) * Distance, FMath::Sin(Rad) * Distance, Height);
		const FVector CamPos = Center + Offset;
		VC->SetViewLocation(CamPos);
		VC->SetViewRotation((Center - CamPos).Rotation());
		VC->Invalidate();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		FMCPJson::WriteVec3(Result, TEXT("camera_location"), CamPos);
		FMCPJson::WriteVec3(Result, TEXT("target"), Center);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Orbit around %s angle=%.1f"), *ActorName, Angle);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleSetFov(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	double Fov = 90.0;
	if (!Request.Params->TryGetNumberField(TEXT("fov"), Fov))
		return InvalidParams(Request.Id, TEXT("Missing 'fov' (degrees)"));

	auto Task = [Fov]() -> TSharedPtr<FJsonObject>
	{
		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));
		VC->ViewFOV = static_cast<float>(Fov);
		VC->Invalidate();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("fov"), Fov);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set viewport FOV to %.1f"), Fov);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleSetViewMode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString Mode;
	if (!Request.Params->TryGetStringField(TEXT("mode"), Mode))
		return InvalidParams(Request.Id, TEXT("Missing 'mode'"));

	auto Task = [Mode]() -> TSharedPtr<FJsonObject>
	{
		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));

		EViewModeIndex Idx = VMI_Lit;
		if (Mode.Equals(TEXT("lit"), ESearchCase::IgnoreCase)) Idx = VMI_Lit;
		else if (Mode.Equals(TEXT("unlit"), ESearchCase::IgnoreCase)) Idx = VMI_Unlit;
		else if (Mode.Equals(TEXT("wireframe"), ESearchCase::IgnoreCase)) Idx = VMI_Wireframe;
		else if (Mode.Equals(TEXT("brush_wireframe"), ESearchCase::IgnoreCase)) Idx = VMI_BrushWireframe;
		else if (Mode.Equals(TEXT("detail_lighting"), ESearchCase::IgnoreCase)) Idx = VMI_Lit_DetailLighting;
		else if (Mode.Equals(TEXT("lighting_only"), ESearchCase::IgnoreCase)) Idx = VMI_LightingOnly;
		else if (Mode.Equals(TEXT("light_complexity"), ESearchCase::IgnoreCase)) Idx = VMI_LightComplexity;
		else if (Mode.Equals(TEXT("shader_complexity"), ESearchCase::IgnoreCase)) Idx = VMI_ShaderComplexity;
		else if (Mode.Equals(TEXT("stationary_light_overlap"), ESearchCase::IgnoreCase)) Idx = VMI_StationaryLightOverlap;
		else if (Mode.Equals(TEXT("lightmap_density"), ESearchCase::IgnoreCase)) Idx = VMI_LightmapDensity;
		else return FMCPJson::MakeError(FString::Printf(TEXT("Unknown view mode: %s"), *Mode));

		VC->SetViewMode(Idx);
		VC->Invalidate();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("mode"), Mode);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set view mode to %s"), *Mode);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleToggleGameView(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));
		VC->SetGameView(!VC->IsInGameView());
		VC->Invalidate();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("game_view"), VC->IsInGameView());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Game view = %d"), VC->IsInGameView());
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleBookmarkSave(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	int32 Index = 0;
	if (!Request.Params->TryGetNumberField(TEXT("index"), Index) || Index < 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'index' (>=0)"));

	auto Task = [Index]() -> TSharedPtr<FJsonObject>
	{
		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));
		IBookmarkTypeTools::Get().CreateOrSetBookmark(static_cast<uint32>(Index), VC);
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("index"), Index);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Saved bookmark %d"), Index);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleBookmarkRestore(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	int32 Index = 0;
	if (!Request.Params->TryGetNumberField(TEXT("index"), Index) || Index < 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'index'"));

	auto Task = [Index]() -> TSharedPtr<FJsonObject>
	{
		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));
		if (!IBookmarkTypeTools::Get().CheckBookmark(static_cast<uint32>(Index), VC))
			return FMCPJson::MakeError(FString::Printf(TEXT("Bookmark %d does not exist"), Index));
		IBookmarkTypeTools::Get().JumpToBookmark(static_cast<uint32>(Index), nullptr, VC);
		VC->Invalidate();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("index"), Index);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Restored bookmark %d"), Index);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleSetGridSnap(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	bool bEnabled = true;
	Request.Params->TryGetBoolField(TEXT("enabled"), bEnabled);
	int32 GridSizeIdx = -1;
	Request.Params->TryGetNumberField(TEXT("grid_size_index"), GridSizeIdx);

	auto Task = [bEnabled, GridSizeIdx]() -> TSharedPtr<FJsonObject>
	{
		ULevelEditorViewportSettings* Settings = GetMutableDefault<ULevelEditorViewportSettings>();
		if (!Settings) return FMCPJson::MakeError(TEXT("LevelEditorViewportSettings not available"));
		Settings->GridEnabled = bEnabled ? 1 : 0;
		if (GridSizeIdx >= 0) Settings->CurrentPosGridSize = GridSizeIdx;
		Settings->PostEditChange();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		Result->SetNumberField(TEXT("grid_size_index"), Settings->CurrentPosGridSize);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Grid snap enabled=%d idx=%d"), bEnabled, Settings->CurrentPosGridSize);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleToggleRealtime(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		FLevelEditorViewportClient* VC = ResolveLevelViewportClient();
		if (!VC) return FMCPJson::MakeError(TEXT("No active viewport client"));
		const bool bRealtimeNow = !VC->IsRealtime();
		VC->SetRealtime(bRealtimeNow);
		VC->Invalidate();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("realtime"), bRealtimeNow);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Realtime = %d"), bRealtimeNow);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleForceRedraw(const FMCPRequest& Request)
{
	bool bInvalidateHitProxies = true;
	if (Request.Params.IsValid())
	{
		FMCPJson::ReadBool(Request.Params, TEXT("invalidate_hit_proxies"), bInvalidateHitProxies);
	}

	auto Task = [bInvalidateHitProxies]() -> TSharedPtr<FJsonObject>
	{
		if (!GEditor)
		{
			return FMCPJson::MakeError(TEXT("GEditor unavailable"));
		}
		// Synchronously repaints all level editor viewports. Commits camera
		// and view changes (including those queued by Python scripts via
		// UnrealEditorSubsystem) before this call returns, so subsequent
		// screenshot/capture sees the new frame.
		GEditor->RedrawLevelEditingViewports(bInvalidateHitProxies);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("invalidate_hit_proxies"), bInvalidateHitProxies);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: viewport/force_redraw (invalidate_hit_proxies=%d)"), bInvalidateHitProxies ? 1 : 0);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FViewportService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// set_location
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_location");
		Tool.Description = TEXT("Move the active Level Editor viewport camera to a world-space location. Returns {success, location:[X,Y,Z]}. "
			"Params: location (array [X,Y,Z] in world-space cm, +X forward/+Y right/+Z up, required). "
			"Workflow: pair with viewport/set_rotation to aim, or use viewport/focus_actor to frame an actor automatically; then call screenshot/capture (it auto-redraws, so no manual viewport/force_redraw is needed). "
			"Warning: only the active Level Editor viewport is repositioned (mutates the user's view). The view data updates immediately but the on-screen pixel repaint waits for the next editor tick; if another consumer reads pixels directly (not screenshot/capture, which redraws itself) call viewport/force_redraw first.");

		TSharedPtr<FJsonObject> LocParam = MakeShared<FJsonObject>();
		LocParam->SetStringField(TEXT("type"), TEXT("array"));
		LocParam->SetStringField(TEXT("description"), TEXT("Camera location as [X, Y, Z] in world-space cm"));
		Tool.Parameters->SetObjectField(TEXT("location"), LocParam);
		Tool.RequiredParams.Add(TEXT("location"));
		
		Tools.Add(Tool);
	}
	
	// set_rotation
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_rotation");
		Tool.Description = TEXT("Aim the active Level Editor viewport camera by setting its rotation. Returns {success, rotation:[Pitch,Yaw,Roll]}. "
			"Params: rotation (array [Pitch,Yaw,Roll] in degrees, required; note the Details panel labels these X=Roll/Y=Pitch/Z=Yaw, so the order differs). "
			"Workflow: pair with viewport/set_location, or use viewport/focus_actor to frame an actor automatically instead of computing angles; then screenshot/capture (it auto-redraws). "
			"Warning: only the active Level Editor viewport is affected. The view data updates immediately but the on-screen repaint waits for the next editor tick; screenshot/capture redraws itself, so a manual viewport/force_redraw is only needed for other direct pixel readers.");

		TSharedPtr<FJsonObject> RotParam = MakeShared<FJsonObject>();
		RotParam->SetStringField(TEXT("type"), TEXT("array"));
		RotParam->SetStringField(TEXT("description"), TEXT("Camera rotation as [Pitch, Yaw, Roll] in degrees"));
		Tool.Parameters->SetObjectField(TEXT("rotation"), RotParam);
		Tool.RequiredParams.Add(TEXT("rotation"));
		
		Tools.Add(Tool);
	}
	
	// get_transform
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_transform");
		Tool.Description = TEXT("Read the active Level Editor viewport camera transform. Returns {success, location:[X,Y,Z] world-space cm, rotation:[Pitch,Yaw,Roll] degrees}. "
			"Params: (none). "
			"Workflow: call before viewport/bookmark_save, or use the result to compute a delta for a follow-up viewport/set_location. "
			"Warning: read-only (does not move the camera); returns success=false with an error when no Level Editor viewport is available.");
		Tools.Add(Tool);
	}
	
	// focus_actor
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("focus_actor");
		Tool.Description = TEXT("Frame an actor in the active Level Editor viewport (like pressing F on a selection). Matches by exact label first, then exact internal name, then case-insensitive substring; returns {success, actor_name (matched label), actor_id (internal name), matched_by:'label'|'name'|'partial'}. "
			"Params: actor_name (string: an actor label, internal name/ID, or partial substring of either, required). "
			"Workflow: list actors first, then focus_actor, then screenshot/capture (it auto-redraws) to confirm the framing; prefer this over computing viewport/set_location + set_rotation by hand. "
			"Warning: moves the active Level Editor viewport camera; a partial/substring match picks the FIRST matching actor, so pass a unique label when several actors share a substring. Returns success=false if no actor matches.");

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("The actor label, internal name/ID, or a partial substring of either to focus on"));
		Tool.Parameters->SetObjectField(TEXT("actor_name"), NameParam);
		Tool.RequiredParams.Add(TEXT("actor_name"));
		
		Tools.Add(Tool);
	}
	
	// trace_from_screen
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("trace_from_screen");
		Tool.Description = TEXT("Deproject a screen-space point in the Level Editor viewport and line-trace it along the camera ray against the Visibility channel (trace length 100000 cm). "
			"Always returns {success, hit, screen_x, screen_y, pixel_x, pixel_y, viewport_width, viewport_height}; on hit adds {location:[X,Y,Z] world cm, normal:[X,Y,Z] surface normal, distance (cm from camera), actor_name, actor_id, actor_class, component_name, physical_material?}; on miss adds {ray_direction:[X,Y,Z]}. "
			"Params: screen_x (number 0-1, fraction from left edge, default 0.5=center); screen_y (number 0-1, fraction from top edge, default 0.5=center). Out-of-range values are clamped to 0-1. "
			"Workflow: screenshot/capture, estimate (x,y) from the image, trace_from_screen, then feed location to a spawn/placement tool and normal to align rotation; for selecting the actor under a point use utility/select_at_screen instead. "
			"Warning: read-only (does not modify the scene); hit:false simply means the ray missed all collidable geometry, not an error -- always check the hit field.");
		
		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Screen X as 0-1 percentage (0=left edge, 0.5=center, 1=right edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_x"), XParam);
		
		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Screen Y as 0-1 percentage (0=top edge, 0.5=center, 1=bottom edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_y"), YParam);

		Tools.Add(Tool);
	}

	// ---------- Phase 1.A additions ----------
	Tools.Add(FMCPToolBuilder(TEXT("orbit_around_actor"),
		TEXT("Place the Level Editor viewport camera on a horizontal ring around an actor's bounds center, looking inward. Returns {success, camera_location:[X,Y,Z], target:[X,Y,Z]}. "
			 "Params: actor_name (string, exact actor label, required); angle (number degrees, 0=+X axis, default 0); distance (number, horizontal radius from target in cm, default 500); height (number, vertical offset above the bounds center in cm, default 200). "
			 "Workflow: call repeatedly with stepped angle values for turntable-style coverage, taking a screenshot/capture (auto-redraws) after each. "
			 "Warning: moves the active Level Editor viewport camera; resolves the actor by exact label only (unlike viewport/focus_actor, which also matches name/substring) and returns an error if not found."))
		.RequiredString(TEXT("actor_name"), TEXT("Exact actor label to orbit around"))
		.OptionalNumber(TEXT("angle"), TEXT("Horizontal angle in degrees (0 = +X axis), default 0"))
		.OptionalNumber(TEXT("distance"), TEXT("Horizontal radius from target in cm (default 500)"))
		.OptionalNumber(TEXT("height"), TEXT("Vertical offset above the bounds center in cm (default 200)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_fov"),
		TEXT("Set the active Level Editor perspective viewport's horizontal field of view. Returns {success, fov}. "
			 "Params: fov (number, horizontal FOV in degrees, required; typical 60-120, editor default 90). "
			 "Workflow: set before screenshot/capture (auto-redraws); lower fov for a tighter/zoomed framing, higher for a wider angle. "
			 "Warning: affects only the perspective viewport (no effect on orthographic views) and persists until changed again or the viewport is reset."))
		.RequiredNumber(TEXT("fov"), TEXT("Horizontal field of view in degrees (typical 60-120, editor default 90)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_view_mode"),
		TEXT("Switch the active Level Editor viewport's render/visualization mode. Returns {success, mode}. "
			 "Params: mode (enum, required) -- one of lit, unlit, wireframe, brush_wireframe, detail_lighting, "
			 "lighting_only, light_complexity, shader_complexity, stationary_light_overlap, lightmap_density. "
			 "Workflow: set a diagnostic mode (e.g. shader_complexity, lightmap_density), then screenshot/capture (auto-redraws); switch back to lit when done. "
			 "Warning: mutates the user's active viewport rendering; note rendering/set_view_mode is a sibling tool with a different mode set (adds reflections, buffer_visualization, lod_coloration, path_tracing)."))
		.RequiredEnum(TEXT("mode"), {
			TEXT("lit"), TEXT("unlit"), TEXT("wireframe"), TEXT("brush_wireframe"),
			TEXT("detail_lighting"), TEXT("lighting_only"), TEXT("light_complexity"),
			TEXT("shader_complexity"), TEXT("stationary_light_overlap"), TEXT("lightmap_density")
		}, TEXT("Viewport render mode"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("toggle_game_view");
		Tool.Description = TEXT("Toggle Game View on the active Level Editor viewport, flipping its current state. Returns {success, game_view (the new state)}. "
			"Params: (none). "
			"Workflow: enable before screenshot/capture (auto-redraws) for a clean in-game framing that hides editor-only billboards, gizmos, and grid; toggle again to restore the editor overlays. "
			"Warning: this is a toggle, not a set -- it flips whatever the current state is, so read game_view in the result rather than assuming a value.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("bookmark_save"),
		TEXT("Save the active Level Editor viewport camera (location, rotation, and view settings) into a numbered bookmark slot. Returns {success, index}. "
			 "Params: index (integer >= 0, required; the bookmark slot to create or overwrite). "
			 "Workflow: capture a good framing, save it here, then restore it later with viewport/bookmark_restore to return to the exact viewpoint. "
			 "Warning: overwrites any existing bookmark at that index without prompting."))
		.RequiredInteger(TEXT("index"), TEXT("Bookmark slot index (integer >= 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("bookmark_restore"),
		TEXT("Jump the active Level Editor viewport camera to a previously saved bookmark. Returns {success, index}. "
			 "Params: index (integer >= 0, required; the slot saved earlier with viewport/bookmark_save). "
			 "Workflow: restore, then screenshot/capture (auto-redraws) to view the recalled framing. "
			 "Warning: moves the active viewport camera; returns an error if no bookmark was ever saved at that index."))
		.RequiredInteger(TEXT("index"), TEXT("Bookmark slot index (integer >= 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_grid_snap"),
		TEXT("Enable or disable editor position grid snapping, optionally selecting the grid size. Returns {success, enabled, grid_size_index (the resulting index)}. "
			 "Params: enabled (bool, required; true snaps to grid, false disables); grid_size_index (integer >= 0, optional; index into the editor's grid-size list -- only applied when provided, negative values are ignored). "
			 "Workflow: set before drag-placing or nudging actors so transforms snap cleanly; read grid_size_index from the result to confirm. "
			 "Warning: mutates the global editor viewport settings (ULevelEditorViewportSettings), affecting all viewports and interactive editing, not just this session."))
		.RequiredBool(TEXT("enabled"), TEXT("true = snap to grid, false = disable snapping"))
		.OptionalInteger(TEXT("grid_size_index"), TEXT("Index into the editor's grid-size list (>= 0; ignored when omitted or negative)"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("toggle_realtime");
		Tool.Description = TEXT("Toggle realtime mode on the active Level Editor viewport, flipping its current state. Returns {success, realtime (the new state)}. "
			"Params: (none). "
			"Workflow: enable so animations, particles, and shader/world updates keep redrawing for a live preview; disable to save CPU when positioning the camera precisely. "
			"Warning: this is a toggle, not a set -- it flips the current state, so read realtime in the result. With realtime off the viewport only repaints on demand; screenshot/capture still auto-redraws regardless.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("force_redraw"),
		TEXT("Synchronously repaint all Level Editor viewports now, committing any queued camera/view changes to pixels before returning. Returns {success, invalidate_hit_proxies}. "
			 "Params: invalidate_hit_proxies (bool, optional, default true; also rebuilds hit proxies so screen-picking stays accurate -- set false for a cheaper repaint when picking is not needed). "
			 "Workflow: chain after a python/execute camera edit (or any tool that warns its repaint is deferred) before a consumer that reads pixels without redrawing itself. "
			 "Warning: screenshot/capture and screenshot/save already auto-redraw, so you do NOT need this before them; use it for other direct pixel readers or to force an immediate on-screen refresh."))
		.OptionalBool(TEXT("invalidate_hit_proxies"), TEXT("Rebuild hit proxies for picking (default true). Set false for a cheaper repaint when picking isn't needed."))
		.Build());

	return Tools;
}

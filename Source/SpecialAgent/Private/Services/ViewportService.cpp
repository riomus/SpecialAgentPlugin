// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ViewportService.h"
#include "MCPCommon/MCPRequestContext.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPActorResolver.h"
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

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
			return Result;
		}

		ViewportClient->SetViewLocation(Location);
		
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

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
			return Result;
		}

		ViewportClient->SetViewRotation(Rotation);
		
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

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
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
		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (ViewportClient)
		{
			ViewportClient->FocusViewportOnBox(FoundActor->GetComponentsBoundingBox());
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

		FViewport* Viewport = GEditor->GetActiveViewport();
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport found"));
			return Result;
		}

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(Viewport->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
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
// Helper: resolve a FLevelEditorViewportClient, or null.
static FLevelEditorViewportClient* ResolveLevelViewportClient()
{
	if (!GEditor) return nullptr;
	FViewport* VP = GEditor->GetActiveViewport();
	if (!VP) return nullptr;
	return static_cast<FLevelEditorViewportClient*>(VP->GetClient());
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
		Tool.Description = TEXT("Set the active editor viewport camera location in world space. Returns {success, location}. "
			"Params: location ([X,Y,Z] cm world, required). "
			"Workflow: pair with viewport/set_rotation to aim; call viewport/force_redraw before screenshot/capture so the captured frame reflects the new camera. "
			"Warning: only the active Level Editor viewport is affected; updates the view data immediately but the pixel repaint happens on the next editor tick unless you call viewport/force_redraw.");
		
		TSharedPtr<FJsonObject> LocParam = MakeShared<FJsonObject>();
		LocParam->SetStringField(TEXT("type"), TEXT("array"));
		LocParam->SetStringField(TEXT("description"), TEXT("Camera location as [X, Y, Z]"));
		Tool.Parameters->SetObjectField(TEXT("location"), LocParam);
		Tool.RequiredParams.Add(TEXT("location"));
		
		Tools.Add(Tool);
	}
	
	// set_rotation
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_rotation");
		Tool.Description = TEXT("Set the active editor viewport camera rotation. Returns {success, rotation}. "
			"Params: rotation ([Pitch,Yaw,Roll] degrees, required). "
			"Workflow: pair with viewport/set_location; use viewport/focus_actor for 'frame this actor' behaviour instead of manual angles. "
			"Warning: updates view data immediately but the pixel repaint happens next editor tick — call viewport/force_redraw before screenshot/capture.");
		
		TSharedPtr<FJsonObject> RotParam = MakeShared<FJsonObject>();
		RotParam->SetStringField(TEXT("type"), TEXT("array"));
		RotParam->SetStringField(TEXT("description"), TEXT("Camera rotation as [Pitch, Yaw, Roll]"));
		Tool.Parameters->SetObjectField(TEXT("rotation"), RotParam);
		Tool.RequiredParams.Add(TEXT("rotation"));
		
		Tools.Add(Tool);
	}
	
	// get_transform
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_transform");
		Tool.Description = TEXT("Get the active editor viewport camera transform. Returns {success, location:[X,Y,Z], rotation:[Pitch,Yaw,Roll]}. "
			"Params: (none). "
			"Workflow: call before bookmark_save, or use the result to compute offsets for subsequent set_location calls.");
		Tools.Add(Tool);
	}
	
	// focus_actor
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("focus_actor");
		Tool.Description = TEXT("Frame an actor in the viewport (like pressing F in the editor). Matches by exact label, internal name, or case-insensitive substring; returns {actor_name, actor_id, matched_by}. "
			"Params: actor_name (string, label/ID or partial, required). "
			"Workflow: world/list_actors -> focus_actor -> viewport/force_redraw -> screenshot/capture to confirm the view.");
		
		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("The actor label or internal name/ID to focus on"));
		Tool.Parameters->SetObjectField(TEXT("actor_name"), NameParam);
		Tool.RequiredParams.Add(TEXT("actor_name"));
		
		Tools.Add(Tool);
	}
	
	// trace_from_screen
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("trace_from_screen");
		Tool.Description = TEXT("Deproject a screen-space point and line-trace against visibility channel. Returns {hit, location, normal, distance, actor_name, actor_class, component_name, physical_material} on hit; {hit:false, ray_direction} on miss. "
			"Params: screen_x (number 0-1, fraction from left, default 0.5); screen_y (number 0-1, fraction from top, default 0.5). "
			"Workflow: screenshot/capture -> estimate (x,y) -> trace_from_screen -> use location to world/spawn_actor and normal to align rotation.");
		
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
		TEXT("Position the camera on a ring around an actor's bounds center, looking inward. Effect: sets viewport location + rotation. "
			 "Params: actor_name (string), angle (number deg, 0=+X), distance (number cm, default 500), height (number cm, default 200). "
			 "Workflow: call viewport/force_redraw before screenshot/capture so the new framing is visible."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label to orbit around"))
		.OptionalNumber(TEXT("angle"), TEXT("Horizontal angle in degrees (0 = +X axis)"))
		.OptionalNumber(TEXT("distance"), TEXT("Horizontal distance from target cm (default 500)"))
		.OptionalNumber(TEXT("height"), TEXT("Vertical offset above target cm (default 200)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_fov"),
		TEXT("Set the perspective camera field of view. Effect: updates FLevelEditorViewportClient::ViewFOV. "
			 "Params: fov (number, degrees, typical 60-120). "
			 "Workflow: call viewport/force_redraw before screenshot/capture so the new FOV is visible."))
		.RequiredNumber(TEXT("fov"), TEXT("FOV in degrees"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_view_mode"),
		TEXT("Switch the viewport's render mode. Effect: one of lit/unlit/wireframe/brush_wireframe/detail_lighting/"
			 "lighting_only/light_complexity/shader_complexity/stationary_light_overlap/lightmap_density. "
			 "Params: mode (enum). "
			 "Workflow: call viewport/force_redraw before screenshot/capture so the new view mode is visible."))
		.RequiredEnum(TEXT("mode"), {
			TEXT("lit"), TEXT("unlit"), TEXT("wireframe"), TEXT("brush_wireframe"),
			TEXT("detail_lighting"), TEXT("lighting_only"), TEXT("light_complexity"),
			TEXT("shader_complexity"), TEXT("stationary_light_overlap"), TEXT("lightmap_density")
		}, TEXT("Viewport render mode"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("toggle_game_view");
		Tool.Description = TEXT("Toggle editor viewport Game View mode. Effect: hides editor-only icons/gizmos if on. "
			"Params: (none). "
			"Workflow: call before screenshot to capture an in-game framing; follow with viewport/force_redraw so the toggle is reflected in the next capture.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("bookmark_save"),
		TEXT("Save the current viewport camera to a bookmark slot. Effect: records location, rotation, and view settings. "
			 "Params: index (integer 0..MaxBookmarks-1). "
			 "Workflow: pair with bookmark_restore to revisit a viewpoint later."))
		.RequiredInteger(TEXT("index"), TEXT("Bookmark slot index (0..max)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("bookmark_restore"),
		TEXT("Restore a saved viewport bookmark. Effect: teleports camera to the saved view. "
			 "Params: index (integer). "
			 "Workflow: call viewport/force_redraw before screenshot/capture so the restored view is visible. "
			 "Warning: returns error if the slot was never saved."))
		.RequiredInteger(TEXT("index"), TEXT("Bookmark slot index"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_grid_snap"),
		TEXT("Enable/disable position grid snap and optionally pick the grid size index. "
			 "Effect: mutates ULevelEditorViewportSettings. "
			 "Params: enabled (bool, required), grid_size_index (integer >=0, optional; indexes into Pow2GridSizes/DecimalGridSizes). "
			 "Workflow: pair with viewport/get_transform to confirm; world/spawn_actor will then snap to the configured grid."))
		.RequiredBool(TEXT("enabled"), TEXT("true=snap to grid, false=disable"))
		.OptionalInteger(TEXT("grid_size_index"), TEXT("Index into the grid size array"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("toggle_realtime");
		Tool.Description = TEXT("Toggle viewport realtime mode. Effect: when on, viewport animates; off saves CPU. "
			"Params: (none). "
			"Workflow: disable when positioning camera precisely, enable for playback preview.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("force_redraw"),
		TEXT("Synchronously repaint all level editor viewports via GEditor->RedrawLevelEditingViewports(true). "
			 "Effect: commits any queued camera/view changes to pixels before this call returns. "
			 "Workflow: chain after python/execute, viewport/set_location, viewport/set_rotation, viewport/focus_actor etc. "
			 "and before screenshot/capture so the captured frame reflects the new state. "
			 "Params: invalidate_hit_proxies (bool, optional, default true — rebuild hit proxies for picking)."))
		.OptionalBool(TEXT("invalidate_hit_proxies"), TEXT("Rebuild hit proxies (default true). Set false for a cheaper repaint when picking isn't needed."))
		.Build());

	return Tools;
}

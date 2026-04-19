// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/UtilityService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPActorResolver.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Editor/Transactor.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Editor/UnrealEdTypes.h"
#include "Selection.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "SceneView.h"
#include "Editor/GroupActor.h"
#include "ActorGroupingUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/MessageDialog.h"
#include "Framework/Docking/TabManager.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

FUtilityService::FUtilityService()
{
}

FString FUtilityService::GetServiceDescription() const
{
	return TEXT("Editor utilities - save, undo/redo, and selection management");
}

FMCPResponse FUtilityService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("save_level")) return HandleSaveLevel(Request);
	if (MethodName == TEXT("undo")) return HandleUndo(Request);
	if (MethodName == TEXT("redo")) return HandleRedo(Request);
	if (MethodName == TEXT("select_actor")) return HandleSelectActor(Request);
	if (MethodName == TEXT("get_selection")) return HandleGetSelection(Request);
	if (MethodName == TEXT("get_selection_bounds")) return HandleGetSelectionBounds(Request);
	if (MethodName == TEXT("select_at_screen")) return HandleSelectAtScreen(Request);

	// Phase 1.A
	if (MethodName == TEXT("focus_asset_in_browser")) return HandleFocusAssetInBrowser(Request);
	if (MethodName == TEXT("deselect_all")) return HandleDeselectAll(Request);
	if (MethodName == TEXT("invert_selection")) return HandleInvertSelection(Request);
	if (MethodName == TEXT("select_by_class")) return HandleSelectByClass(Request);
	if (MethodName == TEXT("group_selected")) return HandleGroupSelected(Request);
	if (MethodName == TEXT("ungroup")) return HandleUngroup(Request);
	if (MethodName == TEXT("begin_transaction")) return HandleBeginTransaction(Request);
	if (MethodName == TEXT("end_transaction")) return HandleEndTransaction(Request);
	if (MethodName == TEXT("show_notification")) return HandleShowNotification(Request);
	if (MethodName == TEXT("show_dialog")) return HandleShowDialog(Request);
	if (MethodName == TEXT("focus_tab")) return HandleFocusTab(Request);

	return MethodNotFound(Request.Id, TEXT("utility"), MethodName);
}

FMCPResponse FUtilityService::HandleSaveLevel(const FMCPRequest& Request)
{
	auto SaveTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Save current level
		bool bSaved = FEditorFileUtils::SaveCurrentLevel();

		Result->SetBoolField(TEXT("success"), bSaved);
		if (bSaved)
		{
			Result->SetStringField(TEXT("message"), TEXT("Level saved successfully"));
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Level saved"));
		}
		else
		{
			Result->SetStringField(TEXT("error"), TEXT("Failed to save level"));
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SaveTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleUndo(const FMCPRequest& Request)
{
	int32 Steps = 1;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("steps"), Steps);
	}

	auto UndoTask = [Steps]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		for (int32 i = 0; i < Steps; i++)
		{
			if (GEditor->Trans->CanUndo())
			{
				GEditor->Trans->Undo();
			}
			else
			{
				break;
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("steps_undone"), Steps);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Undo %d steps"), Steps);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(UndoTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleRedo(const FMCPRequest& Request)
{
	int32 Steps = 1;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("steps"), Steps);
	}

	auto RedoTask = [Steps]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		for (int32 i = 0; i < Steps; i++)
		{
			if (GEditor->Trans->CanRedo())
			{
				GEditor->Trans->Redo();
			}
			else
			{
				break;
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("steps_redone"), Steps);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Redo %d steps"), Steps);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(RedoTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleSelectActor(const FMCPRequest& Request)
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

	bool bAddToSelection = false;
	Request.Params->TryGetBoolField(TEXT("add_to_selection"), bAddToSelection);

	auto SelectTask = [ActorName, bAddToSelection]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Find actor by label
		AActor* FoundActor = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == ActorName)
			{
				FoundActor = *It;
				break;
			}
		}

		if (!FoundActor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
			return Result;
		}

		// Select the actor
		if (!bAddToSelection)
		{
			GEditor->SelectNone(true, true);
		}
		GEditor->SelectActor(FoundActor, true, true);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetBoolField(TEXT("added_to_selection"), bAddToSelection);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Selected actor: %s"), *ActorName);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SelectTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleGetSelection(const FMCPRequest& Request)
{
	auto GetSelTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> SelectedActors;
		
		// Get selected actors using the editor selection set
		USelection* SelectedActorsObj = GEditor->GetSelectedActors();
		if (SelectedActorsObj)
		{
			TArray<AActor*> ActorArray;
			SelectedActorsObj->GetSelectedObjects<AActor>(ActorArray);
			
			for (AActor* Actor : ActorArray)
			{
				if (Actor)
				{
					TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
					ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());
					ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
					
					SelectedActors.Add(MakeShared<FJsonValueObject>(ActorObj));
				}
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("selected_actors"), SelectedActors);
		Result->SetNumberField(TEXT("count"), SelectedActors.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetSelTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleGetSelectionBounds(const FMCPRequest& Request)
{
	auto GetBoundsTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> ActorBoundsArray;
		
		USelection* SelectedActorsObj = GEditor->GetSelectedActors();
		if (SelectedActorsObj)
		{
			TArray<AActor*> ActorArray;
			SelectedActorsObj->GetSelectedObjects<AActor>(ActorArray);
			
			for (AActor* Actor : ActorArray)
			{
				if (!Actor) continue;
				
				TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
				ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());
				ActorObj->SetStringField(TEXT("id"), Actor->GetName());
				ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				
				// Get actor location
				FVector Location = Actor->GetActorLocation();
				TArray<TSharedPtr<FJsonValue>> LocArr;
				LocArr.Add(MakeShared<FJsonValueNumber>(Location.X));
				LocArr.Add(MakeShared<FJsonValueNumber>(Location.Y));
				LocArr.Add(MakeShared<FJsonValueNumber>(Location.Z));
				ActorObj->SetArrayField(TEXT("location"), LocArr);
				
				// Get actor rotation
				FRotator Rotation = Actor->GetActorRotation();
				TArray<TSharedPtr<FJsonValue>> RotArr;
				RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
				RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
				RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
				ActorObj->SetArrayField(TEXT("rotation"), RotArr);
				
				// Get actor scale
				FVector Scale = Actor->GetActorScale3D();
				TArray<TSharedPtr<FJsonValue>> ScaleArr;
				ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.X));
				ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.Y));
				ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.Z));
				ActorObj->SetArrayField(TEXT("scale"), ScaleArr);
				
				// Get forward vector (direction actor is facing)
				FVector ForwardVector = Actor->GetActorForwardVector();
				TArray<TSharedPtr<FJsonValue>> ForwardArr;
				ForwardArr.Add(MakeShared<FJsonValueNumber>(ForwardVector.X));
				ForwardArr.Add(MakeShared<FJsonValueNumber>(ForwardVector.Y));
				ForwardArr.Add(MakeShared<FJsonValueNumber>(ForwardVector.Z));
				ActorObj->SetArrayField(TEXT("forward_vector"), ForwardArr);
				
				// Get right vector
				FVector RightVector = Actor->GetActorRightVector();
				TArray<TSharedPtr<FJsonValue>> RightArr;
				RightArr.Add(MakeShared<FJsonValueNumber>(RightVector.X));
				RightArr.Add(MakeShared<FJsonValueNumber>(RightVector.Y));
				RightArr.Add(MakeShared<FJsonValueNumber>(RightVector.Z));
				ActorObj->SetArrayField(TEXT("right_vector"), RightArr);
				
				// Get up vector
				FVector UpVector = Actor->GetActorUpVector();
				TArray<TSharedPtr<FJsonValue>> UpArr;
				UpArr.Add(MakeShared<FJsonValueNumber>(UpVector.X));
				UpArr.Add(MakeShared<FJsonValueNumber>(UpVector.Y));
				UpArr.Add(MakeShared<FJsonValueNumber>(UpVector.Z));
				ActorObj->SetArrayField(TEXT("up_vector"), UpArr);
				
				// Get bounding box (all components combined)
				FBox BoundingBox = Actor->GetComponentsBoundingBox();
				if (BoundingBox.IsValid)
				{
					TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
					
					// Min point
					TArray<TSharedPtr<FJsonValue>> MinArr;
					MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
					MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
					MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
					BoundsObj->SetArrayField(TEXT("min"), MinArr);
					
					// Max point
					TArray<TSharedPtr<FJsonValue>> MaxArr;
					MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.X));
					MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Y));
					MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Z));
					BoundsObj->SetArrayField(TEXT("max"), MaxArr);
					
					// Center point
					FVector Center = BoundingBox.GetCenter();
					TArray<TSharedPtr<FJsonValue>> CenterArr;
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
					BoundsObj->SetArrayField(TEXT("center"), CenterArr);
					
					// Extent (half-size)
					FVector Extent = BoundingBox.GetExtent();
					TArray<TSharedPtr<FJsonValue>> ExtentArr;
					ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.X));
					ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Y));
					ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Z));
					BoundsObj->SetArrayField(TEXT("extent"), ExtentArr);
					
					// Size (full dimensions)
					FVector Size = BoundingBox.GetSize();
					TArray<TSharedPtr<FJsonValue>> SizeArr;
					SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
					SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
					SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
					BoundsObj->SetArrayField(TEXT("size"), SizeArr);
					
					ActorObj->SetObjectField(TEXT("bounds"), BoundsObj);
				}
				
				ActorBoundsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("actors"), ActorBoundsArray);
		Result->SetNumberField(TEXT("count"), ActorBoundsArray.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got bounds for %d selected actors"), ActorBoundsArray.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetBoundsTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleSelectAtScreen(const FMCPRequest& Request)
{
	// Get screen position as percentage (0.0 to 1.0)
	double ScreenX = 0.5;  // Default to center
	double ScreenY = 0.5;
	bool bAddToSelection = false;
	
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("screen_x"), ScreenX);
		Request.Params->TryGetNumberField(TEXT("screen_y"), ScreenY);
		Request.Params->TryGetBoolField(TEXT("add_to_selection"), bAddToSelection);
	}
	
	// Clamp to valid range
	ScreenX = FMath::Clamp(ScreenX, 0.0, 1.0);
	ScreenY = FMath::Clamp(ScreenY, 0.0, 1.0);

	auto SelectTask = [ScreenX, ScreenY, bAddToSelection]() -> TSharedPtr<FJsonObject>
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
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(SelectAtScreen), true);
		
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

		if (bHit && HitResult.GetActor())
		{
			AActor* HitActor = HitResult.GetActor();
			
			// Select the actor
			if (!bAddToSelection)
			{
				GEditor->SelectNone(true, true);
			}
			GEditor->SelectActor(HitActor, true, true);
			
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), true);
			Result->SetStringField(TEXT("actor_name"), HitActor->GetActorLabel());
			Result->SetStringField(TEXT("actor_id"), HitActor->GetName());
			Result->SetStringField(TEXT("actor_class"), HitActor->GetClass()->GetName());
			
			// Hit location
			TArray<TSharedPtr<FJsonValue>> LocationArr;
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.X));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Y));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Z));
			Result->SetArrayField(TEXT("hit_location"), LocationArr);
			
			// Actor location
			FVector ActorLoc = HitActor->GetActorLocation();
			TArray<TSharedPtr<FJsonValue>> ActorLocArr;
			ActorLocArr.Add(MakeShared<FJsonValueNumber>(ActorLoc.X));
			ActorLocArr.Add(MakeShared<FJsonValueNumber>(ActorLoc.Y));
			ActorLocArr.Add(MakeShared<FJsonValueNumber>(ActorLoc.Z));
			Result->SetArrayField(TEXT("actor_location"), ActorLocArr);
			
			// Actor rotation
			FRotator ActorRot = HitActor->GetActorRotation();
			TArray<TSharedPtr<FJsonValue>> ActorRotArr;
			ActorRotArr.Add(MakeShared<FJsonValueNumber>(ActorRot.Pitch));
			ActorRotArr.Add(MakeShared<FJsonValueNumber>(ActorRot.Yaw));
			ActorRotArr.Add(MakeShared<FJsonValueNumber>(ActorRot.Roll));
			Result->SetArrayField(TEXT("actor_rotation"), ActorRotArr);
			
			// Actor scale
			FVector ActorScale = HitActor->GetActorScale3D();
			TArray<TSharedPtr<FJsonValue>> ActorScaleArr;
			ActorScaleArr.Add(MakeShared<FJsonValueNumber>(ActorScale.X));
			ActorScaleArr.Add(MakeShared<FJsonValueNumber>(ActorScale.Y));
			ActorScaleArr.Add(MakeShared<FJsonValueNumber>(ActorScale.Z));
			Result->SetArrayField(TEXT("actor_scale"), ActorScaleArr);
			
			// Bounding box
			FBox BoundingBox = HitActor->GetComponentsBoundingBox();
			if (BoundingBox.IsValid)
			{
				TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
				
				TArray<TSharedPtr<FJsonValue>> MinArr;
				MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
				MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
				MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
				BoundsObj->SetArrayField(TEXT("min"), MinArr);
				
				TArray<TSharedPtr<FJsonValue>> MaxArr;
				MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.X));
				MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Y));
				MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Z));
				BoundsObj->SetArrayField(TEXT("max"), MaxArr);
				
				FVector Size = BoundingBox.GetSize();
				TArray<TSharedPtr<FJsonValue>> SizeArr;
				SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
				SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
				SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
				BoundsObj->SetArrayField(TEXT("size"), SizeArr);
				
				Result->SetObjectField(TEXT("bounds"), BoundsObj);
			}
			
			// Tags
			TArray<TSharedPtr<FJsonValue>> TagsArr;
			for (const FName& Tag : HitActor->Tags)
			{
				TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			Result->SetArrayField(TEXT("tags"), TagsArr);

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Selected actor at screen (%.2f, %.2f): %s"), 
				ScreenX, ScreenY, *HitActor->GetActorLabel());
		}
		else
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), false);
			Result->SetStringField(TEXT("message"), TEXT("No actor at screen position"));
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: No actor at screen (%.2f, %.2f)"), ScreenX, ScreenY);
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SelectTask);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Phase 1.A additions
// ============================================================================
FMCPResponse FUtilityService::HandleFocusAssetInBrowser(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FAssetData AssetData = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid()) return FMCPJson::MakeError(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

		FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		TArray<FAssetData> Assets;
		Assets.Add(AssetData);
		CB.Get().SyncBrowserToAssets(Assets);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Focused Content Browser on %s"), *AssetPath);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleDeselectAll(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		if (!GEditor) return FMCPJson::MakeError(TEXT("GEditor not available"));
		GEditor->SelectNone(true, true, false);
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Deselected all actors"));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleInvertSelection(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		USelection* Sel = GEditor->GetSelectedActors();
		TSet<AActor*> Currently;
		if (Sel)
		{
			TArray<AActor*> Arr;
			Sel->GetSelectedObjects<AActor>(Arr);
			for (AActor* A : Arr) Currently.Add(A);
		}
		GEditor->SelectNone(true, true, false);
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!Currently.Contains(*It))
			{
				GEditor->SelectActor(*It, true, true);
				++Count;
			}
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("selected"), Count);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Inverted selection (%d now selected)"), Count);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleSelectByClass(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ClassName;
	if (!Request.Params->TryGetStringField(TEXT("class_name"), ClassName))
		return InvalidParams(Request.Id, TEXT("Missing 'class_name'"));
	bool bAdd = false;
	Request.Params->TryGetBoolField(TEXT("add_to_selection"), bAdd);

	auto Task = [ClassName, bAdd]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		UClass* Class = FindFirstObject<UClass>(*ClassName,
			EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!Class) return FMCPJson::MakeError(FString::Printf(TEXT("Class not found: %s"), *ClassName));
		if (!bAdd) GEditor->SelectNone(true, true, false);
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->IsA(Class))
			{
				GEditor->SelectActor(*It, true, true);
				++Count;
			}
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("class_name"), ClassName);
		Result->SetNumberField(TEXT("selected"), Count);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Selected %d actors by class %s"), Count, *ClassName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleGroupSelected(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		if (!GEditor) return FMCPJson::MakeError(TEXT("GEditor not available"));
		UActorGroupingUtils* Utils = UActorGroupingUtils::Get();
		if (!Utils) return FMCPJson::MakeError(TEXT("ActorGroupingUtils not available"));
		UActorGroupingUtils::SetGroupingActive(true);
		AGroupActor* Group = Utils->GroupSelected();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		if (Group)
			Result->SetStringField(TEXT("group_name"), Group->GetActorLabel());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Grouped selected actors"));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleUngroup(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		if (!GEditor) return FMCPJson::MakeError(TEXT("GEditor not available"));
		UActorGroupingUtils* Utils = UActorGroupingUtils::Get();
		if (!Utils) return FMCPJson::MakeError(TEXT("ActorGroupingUtils not available"));
		Utils->UngroupSelected();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Ungrouped selection"));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleBeginTransaction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString Name;
	if (!Request.Params->TryGetStringField(TEXT("name"), Name))
		return InvalidParams(Request.Id, TEXT("Missing 'name'"));

	auto Task = [Name]() -> TSharedPtr<FJsonObject>
	{
		if (!GEditor) return FMCPJson::MakeError(TEXT("GEditor not available"));
		const int32 Index = GEditor->BeginTransaction(FText::FromString(Name));
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("transaction_index"), Index);
		Result->SetStringField(TEXT("name"), Name);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Begin transaction '%s' (idx=%d)"), *Name, Index);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleEndTransaction(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		if (!GEditor) return FMCPJson::MakeError(TEXT("GEditor not available"));
		const int32 Depth = GEditor->EndTransaction();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("depth_after"), Depth);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: End transaction (depth=%d)"), Depth);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleShowNotification(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString Message;
	if (!Request.Params->TryGetStringField(TEXT("message"), Message))
		return InvalidParams(Request.Id, TEXT("Missing 'message'"));
	double Duration = 4.0;
	Request.Params->TryGetNumberField(TEXT("duration"), Duration);

	auto Task = [Message, Duration]() -> TSharedPtr<FJsonObject>
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = static_cast<float>(Duration);
		FSlateNotificationManager::Get().AddNotification(Info);
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("message"), Message);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleShowDialog(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString Message, Title;
	if (!Request.Params->TryGetStringField(TEXT("message"), Message))
		return InvalidParams(Request.Id, TEXT("Missing 'message'"));
	Request.Params->TryGetStringField(TEXT("title"), Title);
	FString TypeStr = TEXT("ok");
	Request.Params->TryGetStringField(TEXT("type"), TypeStr);

	auto Task = [Message, Title, TypeStr]() -> TSharedPtr<FJsonObject>
	{
		EAppMsgType::Type Type = EAppMsgType::Ok;
		if (TypeStr.Equals(TEXT("yes_no"), ESearchCase::IgnoreCase)) Type = EAppMsgType::YesNo;
		else if (TypeStr.Equals(TEXT("ok_cancel"), ESearchCase::IgnoreCase)) Type = EAppMsgType::OkCancel;
		else if (TypeStr.Equals(TEXT("yes_no_cancel"), ESearchCase::IgnoreCase)) Type = EAppMsgType::YesNoCancel;

		const FText TitleText = Title.IsEmpty() ? FText::FromString(TEXT("SpecialAgent")) : FText::FromString(Title);
		EAppReturnType::Type Ret = FMessageDialog::Open(Type, FText::FromString(Message), TitleText);

		FString RetStr;
		switch (Ret)
		{
			case EAppReturnType::Yes: RetStr = TEXT("yes"); break;
			case EAppReturnType::No: RetStr = TEXT("no"); break;
			case EAppReturnType::Cancel: RetStr = TEXT("cancel"); break;
			case EAppReturnType::Ok: RetStr = TEXT("ok"); break;
			case EAppReturnType::YesAll: RetStr = TEXT("yes_all"); break;
			case EAppReturnType::NoAll: RetStr = TEXT("no_all"); break;
			default: RetStr = TEXT("unknown"); break;
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("response"), RetStr);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Dialog answered '%s'"), *RetStr);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleFocusTab(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString TabId;
	if (!Request.Params->TryGetStringField(TEXT("tab_id"), TabId))
		return InvalidParams(Request.Id, TEXT("Missing 'tab_id'"));

	auto Task = [TabId]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FName(*TabId)));
		if (!Tab.IsValid())
			return FMCPJson::MakeError(FString::Printf(TEXT("Tab not found or could not be opened: %s"), *TabId));
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("tab_id"), TabId);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Focused tab %s"), *TabId);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FUtilityService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// save_level
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("save_level");
		Tool.Description = TEXT("Save the current editor level to disk. Persists unsaved actor edits via FEditorFileUtils::SaveCurrentLevel; returns {success, message}. "
			"Workflow: call after world/spawn_actor or bulk edits to commit changes. "
			"Warning: writes to source control if active; may prompt for checkout.");
		Tools.Add(Tool);
	}
	
	// undo
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("undo");
		Tool.Description = TEXT("Undo N editor actions. Rewinds GEditor->Trans up to 'steps' times or until the stack is empty; returns {success, steps_undone}. "
			"Params: steps (integer, count of transactions to undo, default 1). "
			"Workflow: pair with utility/redo. Wrap grouped edits in begin_transaction/end_transaction to undo them atomically.");
		
		TSharedPtr<FJsonObject> StepsParam = MakeShared<FJsonObject>();
		StepsParam->SetStringField(TEXT("type"), TEXT("number"));
		StepsParam->SetStringField(TEXT("description"), TEXT("Number of undo steps (default: 1)"));
		Tool.Parameters->SetObjectField(TEXT("steps"), StepsParam);
		
		Tools.Add(Tool);
	}
	
	// redo
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("redo");
		Tool.Description = TEXT("Redo N previously-undone editor actions. Replays the transactor forward up to 'steps' times; returns {success, steps_redone}. "
			"Params: steps (integer, count of transactions to redo, default 1). "
			"Workflow: pair with utility/undo.");
		
		TSharedPtr<FJsonObject> StepsParam = MakeShared<FJsonObject>();
		StepsParam->SetStringField(TEXT("type"), TEXT("number"));
		StepsParam->SetStringField(TEXT("description"), TEXT("Number of redo steps (default: 1)"));
		Tool.Parameters->SetObjectField(TEXT("steps"), StepsParam);
		
		Tools.Add(Tool);
	}
	
	// select_actor
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("select_actor");
		Tool.Description = TEXT("Select an actor in the editor by label. Highlights it in the viewport/outliner and returns {actor_name, added_to_selection}. "
			"Params: actor_name (string, outliner label, required); add_to_selection (bool, default false — when true, adds to existing selection instead of replacing). "
			"Workflow: pair with utility/get_selection, utility/get_selection_bounds, or viewport/focus_actor to zoom in.");
		
		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("The actor name to select"));
		Tool.Parameters->SetObjectField(TEXT("actor_name"), NameParam);
		Tool.RequiredParams.Add(TEXT("actor_name"));
		
		TSharedPtr<FJsonObject> AddParam = MakeShared<FJsonObject>();
		AddParam->SetStringField(TEXT("type"), TEXT("boolean"));
		AddParam->SetStringField(TEXT("description"), TEXT("Add to current selection instead of replacing (default: false)"));
		Tool.Parameters->SetObjectField(TEXT("add_to_selection"), AddParam);
		
		Tools.Add(Tool);
	}
	
	// get_selection
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_selection");
		Tool.Description = TEXT("Get the currently selected actors in the editor. Returns {selected_actors:[{name,class}], count}. "
			"Workflow: call after utility/select_actor, select_by_class, or select_at_screen to confirm the working set before mutating.");
		Tools.Add(Tool);
	}
	
	// get_selection_bounds
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_selection_bounds");
		Tool.Description = TEXT("Get detailed transform and bounds data for each currently selected actor. Returns per-actor {name, id, class, location, rotation, scale, forward/right/up_vector, bounds:{min,max,center,extent,size}}. "
			"Workflow: select first via utility/select_actor or select_at_screen, then call to size/position follow-up spawns.");
		Tools.Add(Tool);
	}
	
	// select_at_screen
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("select_at_screen");
		Tool.Description = TEXT("Select the actor under a screen-space point by deproject+line-trace. Returns {hit, actor_name, actor_class, hit_location, actor_location/rotation/scale, bounds, tags}. "
			"Params: screen_x (number 0-1, fraction from left edge); screen_y (number 0-1, fraction from top edge); add_to_selection (bool, default false). "
			"Workflow: screenshot/capture -> estimate (x,y) -> select_at_screen -> utility/get_selection_bounds or world/set_actor_* to edit. "
			"Warning: misses when the click hits empty sky; verify hit==true before chaining.");
		
		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Screen X as 0-1 percentage (0=left edge, 0.5=center, 1=right edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_x"), XParam);
		
		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Screen Y as 0-1 percentage (0=top edge, 0.5=center, 1=bottom edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_y"), YParam);
		
		TSharedPtr<FJsonObject> AddParam = MakeShared<FJsonObject>();
		AddParam->SetStringField(TEXT("type"), TEXT("boolean"));
		AddParam->SetStringField(TEXT("description"), TEXT("Add to current selection instead of replacing (default: false)"));
		Tool.Parameters->SetObjectField(TEXT("add_to_selection"), AddParam);

		Tools.Add(Tool);
	}

	// ---------- Phase 1.A additions ----------
	Tools.Add(FMCPToolBuilder(TEXT("focus_asset_in_browser"),
		TEXT("Navigate the Content Browser to an asset. Effect: opens Content Browser and highlights the asset. "
			 "Params: asset_path (string, e.g. /Game/Meshes/Rock.Rock)."))
		.RequiredString(TEXT("asset_path"), TEXT("Full asset path /Game/..."))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("deselect_all");
		Tool.Description = TEXT("Clear the editor selection. Effect: nothing selected. "
			"Workflow: call before select_by_class to avoid accumulating selection.");
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("invert_selection");
		Tool.Description = TEXT("Invert current editor selection. Effect: previously-selected become deselected and vice versa.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("select_by_class"),
		TEXT("Select all actors of a given class. Effect: adds matches to selection (or replaces). "
			 "Params: class_name (string, class name), add_to_selection (bool, optional default false)."))
		.RequiredString(TEXT("class_name"), TEXT("Class name to match"))
		.OptionalBool(TEXT("add_to_selection"), TEXT("Add to current selection instead of replacing"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("group_selected");
		Tool.Description = TEXT("Group currently selected actors into an AGroupActor. Effect: binds them so editor operations move them together.");
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("ungroup");
		Tool.Description = TEXT("Ungroup any selected AGroupActors. Effect: child actors are released and group actors destroyed.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("begin_transaction"),
		TEXT("Open a named undo/redo transaction. Effect: subsequent edits are grouped under 'name' for undo. "
			 "Params: name (string, label shown in undo menu). "
			 "Workflow: pair with end_transaction; everything between is atomic."))
		.RequiredString(TEXT("name"), TEXT("Transaction label"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("end_transaction");
		Tool.Description = TEXT("Close the current undo/redo transaction. Effect: finalizes the group for undo stack.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("show_notification"),
		TEXT("Display a toast notification in the editor. Effect: transient popup visible to the user. "
			 "Params: message (string), duration (number, seconds, optional default 4)."))
		.RequiredString(TEXT("message"), TEXT("Notification text"))
		.OptionalNumber(TEXT("duration"), TEXT("Expire time in seconds (default 4)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("show_dialog"),
		TEXT("Display a modal dialog. Effect: blocks the editor until the user answers; returns the response string. "
			 "Params: message (string), title (string, optional), type (enum 'ok'|'yes_no'|'ok_cancel'|'yes_no_cancel'). "
			 "Warning: modal — use sparingly for user confirmation only."))
		.RequiredString(TEXT("message"), TEXT("Dialog body text"))
		.OptionalString(TEXT("title"), TEXT("Optional dialog title"))
		.OptionalEnum(TEXT("type"), {TEXT("ok"), TEXT("yes_no"), TEXT("ok_cancel"), TEXT("yes_no_cancel")},
			TEXT("Dialog button set (default 'ok')"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("focus_tab"),
		TEXT("Open/focus an editor tab by its tab id. Effect: invokes the named tab in the global tab manager. "
			 "Params: tab_id (string, e.g. 'ContentBrowserTab1', 'LevelEditor', 'OutputLog'). "
			 "Warning: tab ids are internal FNames; use built-in ids to avoid failure."))
		.RequiredString(TEXT("tab_id"), TEXT("Tab FName id, e.g. 'OutputLog'"))
		.Build());

	return Tools;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/UtilityService.h"
#include "MCPCommon/MCPRequestContext.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPViewport.h"
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
#include "Misc/App.h"
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

FMCPResponse FUtilityService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
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
		// A modal dialog blocks the game thread until a human answers, which
		// also blocks the MCP processor Tick and therefore every other in-flight
		// request. Refuse when nobody can answer (headless/unattended) instead of
		// wedging the editor.
		if (FApp::IsUnattended() || !FApp::CanEverRender())
		{
			return FMCPJson::MakeError(TEXT("Cannot show a modal dialog while the editor is unattended/headless — it would block the game thread and all other MCP requests"));
		}

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
		Tool.Description = TEXT("Save the current/persistent editor level to disk via FEditorFileUtils::SaveCurrentLevel, committing unsaved actor and level edits. Returns {success, message} (or {success:false, error}). "
			"Params: (none). "
			"Workflow: call after spawning/transforming actors or other level mutations to persist them; this saves the level package only — modified asset packages (materials, blueprints) are saved through their own save paths, not here. "
			"Warning: writes to disk and, when source control is active, may prompt for checkout; only affects the currently loaded level, not every dirty package.");
		Tools.Add(Tool);
	}
	
	// undo
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("undo");
		Tool.Description = TEXT("Undo recent editor transactions. Rewinds the editor transactor up to 'steps' times, stopping early if the undo stack empties. Returns {success, steps_undone}. "
			"Params: steps (number, count of transactions to undo, default 1). "
			"Workflow: pair with utility/redo; wrap grouped edits in utility/begin_transaction + end_transaction so they undo as one step. "
			"Warning: steps_undone echoes the requested count and does NOT report how many actually rewound when the stack ran out; only transacted operations are undoable (raw Python edits outside a transaction are not).");
		
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
		Tool.Description = TEXT("Redo previously-undone editor transactions. Replays the editor transactor forward up to 'steps' times, stopping early when nothing remains to redo. Returns {success, steps_redone}. "
			"Params: steps (number, count of transactions to redo, default 1). "
			"Workflow: pair with utility/undo to step back and forth through the transaction history. "
			"Warning: steps_redone echoes the requested count and does NOT report how many actually replayed; making any new edit clears the redo stack.");
		
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
		Tool.Description = TEXT("Select a single actor in the editor by its outliner label and highlight it in the viewport/outliner. Returns {success, actor_name, added_to_selection}. "
			"Params: actor_name (string, required, exact outliner label, case-sensitive); add_to_selection (boolean, default false — true adds to the current selection, false replaces it). "
			"Workflow: pair with utility/get_selection or utility/get_selection_bounds to inspect, then viewport/focus_actor to frame it. "
			"Warning: matches the first actor whose label equals actor_name (labels are not guaranteed unique); returns success=false with an error if no actor matches.");
		
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
		Tool.Description = TEXT("List the actors currently selected in the editor. Returns {selected_actors:[{name (outliner label), class}], count}. "
			"Params: (none). "
			"Workflow: call after utility/select_actor, utility/select_by_class, or utility/select_at_screen to confirm the working set before mutating it; use utility/get_selection_bounds for transforms and bounds. "
			"Warning: returns count=0 with an empty list when nothing is selected (not an error).");
		Tools.Add(Tool);
	}
	
	// get_selection_bounds
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_selection_bounds");
		Tool.Description = TEXT("Get detailed transform and world-space bounds for each currently selected actor. Returns {actors:[{name (label), id (internal name), class, location ([X,Y,Z] world cm), rotation ([pitch,yaw,roll] degrees), scale ([X,Y,Z] unitless), forward/right/up_vector (unit), bounds:{min,max,center,extent (half-size),size} all in cm}], count}. "
			"Params: (none). "
			"Workflow: select first via utility/select_actor, utility/select_by_class, or utility/select_at_screen, then call this to size and position follow-up spawns. "
			"Warning: rotation order is [pitch,yaw,roll] (note the Details panel labels these X=Roll/Y=Pitch/Z=Yaw); bounds are omitted for an actor whose components have no valid bounding box; returns count=0 when nothing is selected.");
		Tools.Add(Tool);
	}
	
	// select_at_screen
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("select_at_screen");
		Tool.Description = TEXT("Pick the actor under a screen-space point in the level viewport by deprojecting to a world ray and line-tracing on the Visibility channel (up to ~1000 m). Returns {success, hit, screen_x, screen_y, and on hit: actor_name (label), actor_id, actor_class, hit_location/actor_location ([X,Y,Z] world cm), actor_rotation ([pitch,yaw,roll] degrees), actor_scale, bounds:{min,max,size} (cm), tags}. "
			"Params: screen_x (number 0..1, fraction from the left edge, default 0.5, clamped); screen_y (number 0..1, fraction from the top edge, default 0.5, clamped); add_to_selection (boolean, default false — true adds rather than replaces). "
			"Workflow: screenshot/capture -> estimate (x,y) from the image -> select_at_screen -> utility/get_selection_bounds or a world/set_actor_* tool to edit the hit actor. "
			"Warning: success=true even with no hit (hit=false, e.g. ray hits empty sky or non-colliding geometry) — always check hit==true before chaining; only the primary level-editor perspective viewport is traced.");
		
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
		TEXT("Sync the Content Browser to a single asset, opening the browser and highlighting (selecting) that asset for the user. Returns {success, asset_path}. "
			 "Params: asset_path (string, required, an Unreal object path under /Game/... — e.g. /Game/Meshes/Rock.Rock; resolved through the asset registry, not an OS path). "
			 "Workflow: locate the path via an asset-registry query first, then call this so the user sees it. "
			 "Warning: returns an error if the asset registry has no entry for that path; it highlights but does not load or open the asset."))
		.RequiredString(TEXT("asset_path"), TEXT("Unreal object path under /Game/... e.g. /Game/Meshes/Rock.Rock"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("deselect_all");
		Tool.Description = TEXT("Clear the entire editor actor selection so nothing is selected. Returns {success}. "
			"Params: (none). "
			"Workflow: call before utility/select_by_class or utility/select_actor when you want a clean working set instead of accumulating. "
			"Warning: in-memory selection change only; does not modify or save any actor.");
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("invert_selection");
		Tool.Description = TEXT("Invert the editor actor selection: every level actor that was selected becomes deselected and every other becomes selected. Returns {success, selected (new count)}. "
			"Params: (none). "
			"Workflow: pair with utility/select_by_class to grab the complement set in one shot, then utility/get_selection to read it back. "
			"Warning: operates over all editor-world actors (can select a very large set); in-memory selection only, nothing is mutated or saved.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("select_by_class"),
		TEXT("Select every level actor that is an instance of (or subclass of) a given class. Returns {success, class_name, selected (count)}. "
			 "Params: class_name (string, required, a UClass short name with no U/A prefix, e.g. 'StaticMeshActor' or 'PointLight' — resolved native-first and errors if ambiguous or not found); add_to_selection (boolean, optional, default false — true adds to the current selection, false replaces it). "
			 "Workflow: pair with utility/get_selection or utility/get_selection_bounds to inspect the matches, or utility/group_selected to bind them. "
			 "Warning: matches subclasses too; in-memory selection only, no mutation; errors if class_name does not resolve to a loaded UClass."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name to match, e.g. 'StaticMeshActor' (subclasses included)"))
		.OptionalBool(TEXT("add_to_selection"), TEXT("Add to current selection instead of replacing (default false)"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("group_selected");
		Tool.Description = TEXT("Group the currently selected actors into an AGroupActor so editor operations move/transform them together. Returns {success, group_name (label of the new group, present only when a group was created)}. "
			"Params: (none). "
			"Workflow: select actors first via utility/select_actor or utility/select_by_class; reverse with utility/ungroup. "
			"Warning: enables actor grouping in the editor as a side effect; needs at least two selected actors to form a group (group_name is omitted otherwise).");
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("ungroup");
		Tool.Description = TEXT("Ungroup any AGroupActors in the current selection, releasing their child actors and destroying the group actors. Returns {success}. "
			"Params: (none). "
			"Workflow: the inverse of utility/group_selected; select the group (or its members) first. "
			"Warning: child actors keep their world transforms; no-op (still success) when nothing grouped is selected.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("begin_transaction"),
		TEXT("Open a named editor undo/redo transaction so subsequent edits are grouped under one undo entry. Returns {success, transaction_index, name}. "
			 "Params: name (string, required, label shown in the editor's undo menu). "
			 "Workflow: bracket a group of mutating tool calls with begin_transaction then utility/end_transaction; utility/undo then rewinds them as a single atomic step. "
			 "Warning: you MUST call end_transaction to close it — leaving a transaction open can wedge later edits; transactions can nest, so balance begin/end calls."))
		.RequiredString(TEXT("name"), TEXT("Undo-menu label for this transaction"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("end_transaction");
		Tool.Description = TEXT("Close the current editor undo/redo transaction, finalizing everything opened since the matching begin as one undo step. Returns {success, depth_after (remaining open transaction depth)}. "
			"Params: (none). "
			"Workflow: must follow utility/begin_transaction; once depth_after reaches 0 the group is committed to the undo stack. "
			"Warning: calling without an open transaction is a no-op; with nested transactions you must call end_transaction once per begin_transaction.");
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("show_notification"),
		TEXT("Display a transient toast notification in the editor (Slate notification list) visible to the user. Returns {success, message}. "
			 "Params: message (string, required, text to show); duration (number, seconds the toast stays visible before auto-expiring, optional, default 4). "
			 "Workflow: use to surface scripted-step progress or completion to the user; non-blocking, returns immediately. "
			 "Warning: purely informational with no user response; for a question that needs an answer use utility/show_dialog; nothing is shown in a headless editor."))
		.RequiredString(TEXT("message"), TEXT("Notification text to display"))
		.OptionalNumber(TEXT("duration"), TEXT("Seconds before the toast expires (default 4)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("show_dialog"),
		TEXT("Display a modal message dialog and block until the user picks a button. Returns {success, response} where response is one of 'ok'|'yes'|'no'|'cancel' (matching the chosen button set). "
			 "Params: message (string, required, dialog body); title (string, optional, default 'SpecialAgent'); type (enum 'ok'|'yes_no'|'ok_cancel'|'yes_no_cancel', optional, default 'ok' — selects the button set). "
			 "Workflow: use only when you genuinely need a human decision; for non-blocking status use utility/show_notification instead. "
			 "Warning: modal — blocks the game thread (and therefore ALL other in-flight MCP requests) until answered; returns an error instead of opening when the editor is unattended/headless (FApp::IsUnattended or cannot render)."))
		.RequiredString(TEXT("message"), TEXT("Dialog body text"))
		.OptionalString(TEXT("title"), TEXT("Dialog title (default 'SpecialAgent')"))
		.OptionalEnum(TEXT("type"), {TEXT("ok"), TEXT("yes_no"), TEXT("ok_cancel"), TEXT("yes_no_cancel")},
			TEXT("Button set: ok | yes_no | ok_cancel | yes_no_cancel (default 'ok')"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("focus_tab"),
		TEXT("Open or focus an editor tab by its registered tab id via the global tab manager. Returns {success, tab_id}. "
			 "Params: tab_id (string, required, the internal FName of the tab — e.g. 'OutputLog', 'LevelEditor', 'ContentBrowserTab1'). "
			 "Workflow: use to surface a panel to the user before or after a related action (e.g. focus 'OutputLog' to show log output). "
			 "Warning: tab ids are internal FNames, not display titles; an unregistered or unopenable id returns an error."))
		.RequiredString(TEXT("tab_id"), TEXT("Internal tab FName, e.g. 'OutputLog' or 'LevelEditor'"))
		.Build());

	return Tools;
}

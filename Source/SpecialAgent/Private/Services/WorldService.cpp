// Copyright Epic Games, Inc. All Rights Reserved.
// WorldService Implementation - Core world/actor manipulation methods

#include "Services/WorldService.h"
#include "MCPCommon/MCPRequestContext.h"
#include "Services/PythonService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPActorResolver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "EditorLevelLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Blueprint.h"
#include "Selection.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/UnrealType.h"
#include "CollisionQueryParams.h"
#include "Math/UnrealMathUtility.h"
#include "ScopedTransaction.h"

FWorldService::FWorldService()
{
}

FString FWorldService::GetServiceDescription() const
{
	return TEXT("World and actor manipulation - query, spawn, modify, and organize actors");
}

// Helper function to execute Python code from request params (kept for backward compat)
FMCPResponse FWorldService::ExecutePythonFromParams(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid() || !Request.Params->HasField(TEXT("code")))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter: 'code' (Python script)"));
	}

	FString Code = Request.Params->GetStringField(TEXT("code"));
	float Timeout = Request.Params->HasField(TEXT("timeout")) ? Request.Params->GetNumberField(TEXT("timeout")) : 30.0f;

	FPythonService PythonService;
	TSharedPtr<FJsonObject> PythonParams = MakeShared<FJsonObject>();
	PythonParams->SetStringField(TEXT("code"), Code);
	PythonParams->SetNumberField(TEXT("timeout"), Timeout);

	FMCPRequest PythonRequest;
	PythonRequest.JsonRpc = Request.JsonRpc;
	PythonRequest.Id = Request.Id;
	PythonRequest.Method = TEXT("python/execute");
	PythonRequest.Params = PythonParams;

	return PythonService.HandleExecute(PythonRequest);
}

// Serialize via shared helper + keep local wrapper for return shape
static TSharedPtr<FJsonObject> SerializeActor(AActor* Actor)
{
	if (!Actor) return nullptr;
	TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
	FMCPJson::WriteActor(ActorObj, Actor);
	return ActorObj;
}

// Internal helper: spawn one actor (same logic as HandleSpawnActor), returns new actor ptr + optional error.
// Must be called on the game thread.
static AActor* SpawnActorInternal(UWorld* World, const FString& ActorClass, const FVector& Location,
                                  const FRotator& Rotation, const FVector& Scale, FString& OutType, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("No editor world");
		return nullptr;
	}

	AActor* NewActor = nullptr;

	const bool bIsAssetPath = ActorClass.Contains(TEXT("/Game/")) ||
	                          ActorClass.Contains(TEXT("/Engine/")) ||
	                          ActorClass.StartsWith(TEXT("/"));

	if (bIsAssetPath)
	{
		UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *ActorClass);
		if (StaticMesh)
		{
			FActorSpawnParameters SpawnParams;
			AStaticMeshActor* MeshActor = World->SpawnActor<AStaticMeshActor>(
				AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
			if (MeshActor)
			{
				if (UStaticMeshComponent* MeshComp = MeshActor->GetStaticMeshComponent())
				{
					MeshComp->SetStaticMesh(StaticMesh);
				}
				NewActor = MeshActor;
				OutType = TEXT("StaticMesh");
			}
		}
		else
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ActorClass);
			if (Blueprint && Blueprint->GeneratedClass)
			{
				FActorSpawnParameters SpawnParams;
				NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, Location, FRotator::ZeroRotator, SpawnParams);
				OutType = TEXT("Blueprint");
			}
		}
	}

	if (!NewActor)
	{
		UClass* Class = FindFirstObject<UClass>(*ActorClass,
			EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (Class)
		{
			FActorSpawnParameters SpawnParams;
			NewActor = World->SpawnActor<AActor>(Class, Location, FRotator::ZeroRotator, SpawnParams);
			OutType = TEXT("Class");
		}
	}

	if (!NewActor)
	{
		OutError = FString::Printf(TEXT("Failed to spawn actor from: %s. For meshes, use full path like /Game/Meshes/MyMesh.MyMesh"), *ActorClass);
		return nullptr;
	}

	NewActor->SetActorRotation(Rotation);
	NewActor->SetActorScale3D(Scale);
	NewActor->MarkPackageDirty();
	return NewActor;
}

// ============================================================================
// Request Router
// ============================================================================
FMCPResponse FWorldService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	// Query methods
	if (MethodName == TEXT("list_actors")) return HandleListActors(Request);
	if (MethodName == TEXT("get_actor")) return HandleGetActor(Request);
	if (MethodName == TEXT("find_actors_by_tag")) return HandleFindActorsByTag(Request);
	if (MethodName == TEXT("get_level_info")) return HandleGetLevelInfo(Request);

	// Spawn/Delete methods
	if (MethodName == TEXT("spawn_actor")) return HandleSpawnActor(Request);
	if (MethodName == TEXT("spawn_actors_batch")) return HandleSpawnActorsBatch(Request, Ctx);
	if (MethodName == TEXT("delete_actor")) return HandleDeleteActor(Request);
	if (MethodName == TEXT("delete_actors_batch")) return HandleDeleteActorsBatch(Request, Ctx);
	if (MethodName == TEXT("duplicate_actor")) return HandleDuplicateActor(Request);

	// Transform methods
	if (MethodName == TEXT("set_actor_transform")) return HandleSetActorTransform(Request);
	if (MethodName == TEXT("set_actor_location")) return HandleSetActorLocation(Request);
	if (MethodName == TEXT("set_actor_rotation")) return HandleSetActorRotation(Request);
	if (MethodName == TEXT("set_actor_scale")) return HandleSetActorScale(Request);

	// Property methods
	if (MethodName == TEXT("set_actor_property")) return HandleSetActorProperty(Request);
	if (MethodName == TEXT("set_actor_label")) return HandleSetActorLabel(Request);
	if (MethodName == TEXT("set_actor_material")) return HandleSetActorMaterial(Request);
	if (MethodName == TEXT("set_material_parameter")) return HandleSetMaterialParameter(Request);

	// Organization methods
	if (MethodName == TEXT("create_folder")) return HandleCreateFolder(Request);
	if (MethodName == TEXT("move_actor_to_folder")) return HandleMoveActorToFolder(Request);
	if (MethodName == TEXT("add_actor_tag")) return HandleAddActorTag(Request);
	if (MethodName == TEXT("remove_actor_tag")) return HandleRemoveActorTag(Request);

	// Spatial analysis methods
	if (MethodName == TEXT("measure_distance")) return HandleMeasureDistance(Request);
	if (MethodName == TEXT("find_actors_in_radius")) return HandleFindActorsInRadius(Request);
	if (MethodName == TEXT("find_actors_in_bounds")) return HandleFindActorsInBounds(Request);
	if (MethodName == TEXT("raycast")) return HandleRaycast(Request);
	if (MethodName == TEXT("get_ground_height")) return HandleGetGroundHeight(Request);

	// Pattern placement methods
	if (MethodName == TEXT("place_in_grid")) return HandlePlaceInGrid(Request, Ctx);
	if (MethodName == TEXT("place_along_spline")) return HandlePlaceAlongSpline(Request);
	if (MethodName == TEXT("place_in_circle")) return HandlePlaceInCircle(Request);
	if (MethodName == TEXT("scatter_in_area")) return HandleScatterInArea(Request, Ctx);

	// Actor state methods
	if (MethodName == TEXT("set_actor_tick_enabled")) return HandleSetActorTickEnabled(Request);
	if (MethodName == TEXT("set_actor_hidden")) return HandleSetActorHidden(Request);
	if (MethodName == TEXT("set_actor_collision")) return HandleSetActorCollision(Request);
	if (MethodName == TEXT("attach_to")) return HandleAttachTo(Request);
	if (MethodName == TEXT("detach")) return HandleDetach(Request);

	return MethodNotFound(Request.Id, TEXT("world"), MethodName);
}

// ============================================================================
// Queries
// ============================================================================
FMCPResponse FWorldService::HandleListActors(const FMCPRequest& Request)
{
	int32 MaxResults = 1000;
	FString ClassFilter;

	if (Request.Params.IsValid())
	{
		// Documented flat form: { "class_filter": ..., "max_results": ... }
		Request.Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Request.Params->TryGetNumberField(TEXT("max_results"), MaxResults);

		// Backward-compatible nested form: { "filter": { "class": ..., "max_results": ... } }
		const TSharedPtr<FJsonObject>* FilterObj;
		if (Request.Params->TryGetObjectField(TEXT("filter"), FilterObj))
		{
			(*FilterObj)->TryGetNumberField(TEXT("max_results"), MaxResults);
			(*FilterObj)->TryGetStringField(TEXT("class"), ClassFilter);
		}
	}

	auto Task = [MaxResults, ClassFilter]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		TArray<TSharedPtr<FJsonValue>> ActorsJson;
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It && Count < MaxResults; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter)) continue;
			TSharedPtr<FJsonObject> ActorData = SerializeActor(Actor);
			if (ActorData.IsValid())
			{
				ActorsJson.Add(MakeShared<FJsonValueObject>(ActorData));
				Count++;
			}
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), ActorsJson);
		Result->SetNumberField(TEXT("count"), ActorsJson.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Listed %d actors"), ActorsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleGetActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

	auto Task = [ActorName]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleFindActorsByTag(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString Tag;
	if (!Request.Params->TryGetStringField(TEXT("tag"), Tag))
		return InvalidParams(Request.Id, TEXT("Missing 'tag'"));

	auto Task = [Tag]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		TArray<AActor*> Actors = FMCPActorResolver::ByTag(World, FName(*Tag));

		TArray<TSharedPtr<FJsonValue>> ActorsJson;
		for (AActor* Actor : Actors)
		{
			if (Actor)
				ActorsJson.Add(MakeShared<FJsonValueObject>(SerializeActor(Actor)));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), ActorsJson);
		Result->SetNumberField(TEXT("count"), ActorsJson.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Found %d actors with tag '%s'"), ActorsJson.Num(), *Tag);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleGetLevelInfo(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("level_name"), World->GetMapName());
		Result->SetStringField(TEXT("level_path"), World->GetPathName());

		int32 ActorCount = 0;
		FBox LevelBounds(ForceInit);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			ActorCount++;
			AActor* Actor = *It;
			if (Actor && !Actor->IsA<AWorldSettings>())
				LevelBounds += Actor->GetComponentsBoundingBox(true);
		}
		Result->SetNumberField(TEXT("actor_count"), ActorCount);

		if (LevelBounds.IsValid)
		{
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			FMCPJson::WriteVec3(BoundsObj, TEXT("min"), LevelBounds.Min);
			FMCPJson::WriteVec3(BoundsObj, TEXT("max"), LevelBounds.Max);
			FMCPJson::WriteVec3(BoundsObj, TEXT("center"), LevelBounds.GetCenter());
			FMCPJson::WriteVec3(BoundsObj, TEXT("size"), LevelBounds.GetSize());
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
		}
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Spawn/Delete
// ============================================================================
FMCPResponse FWorldService::HandleSpawnActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorClass;
	if (!Request.Params->TryGetStringField(TEXT("actor_class"), ActorClass))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_class'"));

	FVector Location;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'location'"));

	FRotator Rotation(0, 0, 0);
	FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

	FVector Scale(1, 1, 1);
	FMCPJson::ReadVec3(Request.Params, TEXT("scale"), Scale);

	auto Task = [ActorClass, Location, Rotation, Scale]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn actor")));
		FString SpawnedType, Err;
		AActor* NewActor = SpawnActorInternal(World, ActorClass, Location, Rotation, Scale, SpawnedType, Err);
		if (!NewActor) return FMCPJson::MakeError(Err);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("spawned_type"), SpawnedType);
		Result->SetObjectField(TEXT("actor"), SerializeActor(NewActor));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Spawned %s actor: %s from %s"),
			*SpawnedType, *NewActor->GetActorLabel(), *ActorClass);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSpawnActorsBatch(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	const TArray<TSharedPtr<FJsonValue>>* SpawnsArr = nullptr;
	if (!Request.Params->TryGetArrayField(TEXT("spawns"), SpawnsArr))
		return InvalidParams(Request.Id, TEXT("Missing 'spawns' array"));

	// Copy out spawn specs as plain data so the lambda is self-contained.
	struct FSpawnSpec { FString ActorClass; FVector Loc; FRotator Rot; FVector Scale; };
	TArray<FSpawnSpec> Specs;
	for (const TSharedPtr<FJsonValue>& V : *SpawnsArr)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!V->TryGetObject(Obj)) continue;
		FSpawnSpec S;
		if (!(*Obj)->TryGetStringField(TEXT("actor_class"), S.ActorClass)) continue;
		if (!FMCPJson::ReadVec3(*Obj, TEXT("location"), S.Loc)) continue;
		S.Rot = FRotator::ZeroRotator;
		FMCPJson::ReadRotator(*Obj, TEXT("rotation"), S.Rot);
		S.Scale = FVector(1, 1, 1);
		FMCPJson::ReadVec3(*Obj, TEXT("scale"), S.Scale);
		Specs.Add(S);
	}

	auto SendProgress = Ctx.SendProgress;
	auto Task = [Specs, SendProgress]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn actors batch")));
		const int32 Total = Specs.Num();
		TArray<TSharedPtr<FJsonValue>> Spawned;
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (int32 i = 0; i < Total; ++i)
		{
			const FSpawnSpec& S = Specs[i];
			FString Type, Err;
			AActor* A = SpawnActorInternal(World, S.ActorClass, S.Loc, S.Rot, S.Scale, Type, Err);
			if (A)
				Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
			else
				Errors.Add(MakeShared<FJsonValueString>(Err));
			if ((i + 1) % 4 == 0 || (i + 1) == Total)
				SendProgress((i + 1.0) / (double)Total, 1.0,
					FString::Printf(TEXT("spawn_actors_batch %d/%d"), i + 1, Total));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), Spawned);
		Result->SetNumberField(TEXT("spawned"), Spawned.Num());
		Result->SetArrayField(TEXT("errors"), Errors);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Batch spawned %d actors (%d errors)"), Spawned.Num(), Errors.Num());
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleDeleteActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

	auto Task = [ActorName]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: delete actor")));
		Actor->Modify();
		World->DestroyActor(Actor);
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Deleted actor: %s"), *ActorName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleDeleteActorsBatch(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
	if (!Request.Params->TryGetArrayField(TEXT("actor_names"), NamesArr))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_names' array"));

	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& V : *NamesArr)
	{
		FString S;
		if (V->TryGetString(S)) Names.Add(S);
	}

	auto SendProgress = Ctx.SendProgress;
	auto Task = [Names, SendProgress]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: delete actors batch")));
		const int32 Total = Names.Num();
		int32 Deleted = 0;
		TArray<TSharedPtr<FJsonValue>> NotFound;
		for (int32 i = 0; i < Total; ++i)
		{
			const FString& Name = Names[i];
			AActor* A = FMCPActorResolver::ByLabel(World, Name);
			if (A)
			{
				A->Modify();
				World->DestroyActor(A);
				Deleted++;
			}
			else
			{
				NotFound.Add(MakeShared<FJsonValueString>(Name));
			}
			SendProgress((i + 1.0) / (double)Total, 1.0,
				FString::Printf(TEXT("delete_actors_batch %d/%d"), i + 1, Total));
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("deleted"), Deleted);
		Result->SetArrayField(TEXT("not_found"), NotFound);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Batch deleted %d actors (%d missing)"), Deleted, NotFound.Num());
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleDuplicateActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

	FVector NewLocation(0, 0, 0);
	bool bHasNewLocation = FMCPJson::ReadVec3(Request.Params, TEXT("new_location"), NewLocation);

	auto Task = [ActorName, NewLocation, bHasNewLocation]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		AActor* SourceActor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!SourceActor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: duplicate actor")));

		// Capture the user's current selection so we can restore it after the duplicate op clobbers it.
		TArray<AActor*> PriorSelection;
		if (USelection* CurrentSelection = GEditor->GetSelectedActors())
			CurrentSelection->GetSelectedObjects<AActor>(PriorSelection);

		GEditor->SelectNone(true, true, false);
		GEditor->SelectActor(SourceActor, true, true, true);
		GEditor->edactDuplicateSelected(World->GetCurrentLevel(), false);

		AActor* NewActor = nullptr;
		USelection* Selection = GEditor->GetSelectedActors();
		if (Selection && Selection->Num() > 0)
			NewActor = Cast<AActor>(Selection->GetSelectedObject(0));

		// Restore the exact prior selection regardless of duplicate success/failure.
		GEditor->SelectNone(true, true, false);
		for (AActor* Saved : PriorSelection)
		{
			if (Saved)
				GEditor->SelectActor(Saved, true, false, true);
		}
		GEditor->NoteSelectionChange();

		if (!NewActor || NewActor == SourceActor)
			return FMCPJson::MakeError(TEXT("Failed to duplicate actor"));

		if (bHasNewLocation) NewActor->SetActorLocation(NewLocation);
		NewActor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(NewActor));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Duplicated %s -> %s"), *ActorName, *NewActor->GetActorLabel());
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Transforms
// ============================================================================
FMCPResponse FWorldService::HandleSetActorTransform(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

	FVector Location(0, 0, 0);
	FRotator Rotation(0, 0, 0);
	FVector Scale(1, 1, 1);
	const bool bLoc = FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);
	const bool bRot = FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);
	const bool bScale = FMCPJson::ReadVec3(Request.Params, TEXT("scale"), Scale);
	if (!bLoc && !bRot && !bScale)
		return InvalidParams(Request.Id, TEXT("Provide at least one of location/rotation/scale"));

	auto Task = [ActorName, Location, Rotation, Scale, bLoc, bRot, bScale]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor transform")));
		Actor->Modify();
		FTransform T = Actor->GetActorTransform();
		if (bLoc) T.SetLocation(Location);
		if (bRot) T.SetRotation(Rotation.Quaternion());
		if (bScale) T.SetScale3D(Scale);
		Actor->SetActorTransform(T);
		Actor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set transform for %s"), *ActorName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorLocation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	FVector Location;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'location'"));

	auto Task = [ActorName, Location]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor location")));
		Actor->Modify();
		Actor->SetActorLocation(Location);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorRotation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	FRotator Rotation;
	if (!FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation))
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'rotation' [Pitch, Yaw, Roll]"));

	auto Task = [ActorName, Rotation]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor rotation")));
		Actor->Modify();
		Actor->SetActorRotation(Rotation);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set rotation for %s"), *ActorName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorScale(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	FVector Scale;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("scale"), Scale))
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'scale'"));

	auto Task = [ActorName, Scale]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor scale")));
		Actor->Modify();
		Actor->SetActorScale3D(Scale);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Properties
// ============================================================================
FMCPResponse FWorldService::HandleSetActorProperty(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, PropertyName, Value;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("property_name"), PropertyName))
		return InvalidParams(Request.Id, TEXT("Missing 'property_name'"));
	if (!Request.Params->TryGetStringField(TEXT("value"), Value))
		return InvalidParams(Request.Id, TEXT("Missing 'value' (stringified)"));

	auto Task = [ActorName, PropertyName, Value]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		FProperty* Prop = FindFProperty<FProperty>(Actor->GetClass(), *PropertyName);
		if (!Prop) return FMCPJson::MakeError(FString::Printf(TEXT("Property not found: %s"), *PropertyName));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor property")));
		Actor->Modify();
		void* Addr = Prop->ContainerPtrToValuePtr<void>(Actor);
		const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, Addr, Actor, PPF_None);
		if (!ImportResult)
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to import value '%s' into property '%s'"), *Value, *PropertyName));

		Actor->PostEditChange();
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), Value);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set %s.%s = %s"), *ActorName, *PropertyName, *Value);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorLabel(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, NewLabel;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("new_label"), NewLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'new_label'"));

	auto Task = [ActorName, NewLabel]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor label")));
		Actor->Modify();
		Actor->SetActorLabel(NewLabel);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("old_label"), ActorName);
		Result->SetStringField(TEXT("new_label"), Actor->GetActorLabel());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Relabelled %s -> %s"), *ActorName, *Actor->GetActorLabel());
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorMaterial(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, MaterialPath;
	int32 SlotIndex = 0;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path'"));
	Request.Params->TryGetNumberField(TEXT("slot_index"), SlotIndex);

	auto Task = [ActorName, MaterialPath, SlotIndex]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Mat) return FMCPJson::MakeError(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));

		TArray<UMeshComponent*> MeshComps;
		Actor->GetComponents<UMeshComponent>(MeshComps);
		if (MeshComps.Num() == 0)
			return FMCPJson::MakeError(TEXT("Actor has no MeshComponent"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor material")));
		int32 Applied = 0;
		for (UMeshComponent* MC : MeshComps)
		{
			MC->Modify();
			MC->SetMaterial(SlotIndex, Mat);
			++Applied;
		}
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("components_updated"), Applied);
		Result->SetNumberField(TEXT("slot_index"), SlotIndex);
		Result->SetStringField(TEXT("material_path"), MaterialPath);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set material on %s (slot %d) -> %s"), *ActorName, SlotIndex, *MaterialPath);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetMaterialParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, ParamName, ParamType;
	int32 SlotIndex = 0;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParamName))
		return InvalidParams(Request.Id, TEXT("Missing 'parameter_name'"));
	if (!Request.Params->TryGetStringField(TEXT("parameter_type"), ParamType))
		return InvalidParams(Request.Id, TEXT("Missing 'parameter_type' (scalar|vector)"));
	Request.Params->TryGetNumberField(TEXT("slot_index"), SlotIndex);

	double ScalarValue = 0.0;
	FLinearColor VectorValue = FLinearColor::White;
	const bool bScalar = ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase);
	const bool bVector = ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase);
	if (bScalar)
	{
		if (!Request.Params->TryGetNumberField(TEXT("value"), ScalarValue))
			return InvalidParams(Request.Id, TEXT("Missing numeric 'value' for scalar"));
	}
	else if (bVector)
	{
		if (!FMCPJson::ReadColor(Request.Params, TEXT("value"), VectorValue))
			return InvalidParams(Request.Id, TEXT("Missing array 'value' [R,G,B,A] for vector"));
	}
	else
	{
		return InvalidParams(Request.Id, TEXT("'parameter_type' must be 'scalar' or 'vector'"));
	}

	auto Task = [ActorName, ParamName, bScalar, ScalarValue, VectorValue, SlotIndex]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

		TArray<UMeshComponent*> MeshComps;
		Actor->GetComponents<UMeshComponent>(MeshComps);
		if (MeshComps.Num() == 0) return FMCPJson::MakeError(TEXT("Actor has no MeshComponent"));

		int32 Updated = 0;
		bool bPersisted = false;
		for (UMeshComponent* MC : MeshComps)
		{
			UMaterialInterface* CurrentMat = MC->GetMaterial(SlotIndex);
			if (!CurrentMat) continue;

			// If the slot's material is a MaterialInstanceConstant asset, edit it persistently
			// via the EditorOnly setters so the change survives a save (and shows in the MIC asset).
			if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(CurrentMat))
			{
				const FScopedTransaction PersistTransaction(FText::FromString(TEXT("SpecialAgent: set material parameter (persistent)")));
				MIC->Modify();
				// Note: brace-init (not parens) to avoid the most-vexing-parse — the
				// FName(*ParamName) temporary would otherwise be read as a function decl.
				const FName ParamFName(*ParamName);
				const FMaterialParameterInfo ParamInfo{ ParamFName };
				if (bScalar) MIC->SetScalarParameterValueEditorOnly(ParamInfo, static_cast<float>(ScalarValue));
				else         MIC->SetVectorParameterValueEditorOnly(ParamInfo, VectorValue);
				MIC->PostEditChange();
				MIC->MarkPackageDirty();
				bPersisted = true;
				++Updated;
				continue;
			}

			// Otherwise create (or reuse) a transient dynamic MID so editor-time writes are cheap and non-destructive.
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(CurrentMat);
			if (!MID)
			{
				MID = UMaterialInstanceDynamic::Create(CurrentMat, MC);
				MC->SetMaterial(SlotIndex, MID);
			}
			if (!MID) continue;

			if (bScalar) MID->SetScalarParameterValue(FName(*ParamName), static_cast<float>(ScalarValue));
			else         MID->SetVectorParameterValue(FName(*ParamName), VectorValue);
			++Updated;
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("components_updated"), Updated);
		Result->SetBoolField(TEXT("persisted"), bPersisted);
		if (!bPersisted)
			Result->SetStringField(TEXT("note"), TEXT("Slot material is not a MaterialInstanceConstant; wrote to a transient dynamic instance (not saved). Use material/* tools to edit a MaterialInstanceConstant for persistent changes."));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set material parameter %s on %s (%d comps, persisted=%s)"),
			*ParamName, *ActorName, Updated, bPersisted ? TEXT("true") : TEXT("false"));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Organization
// ============================================================================
FMCPResponse FWorldService::HandleCreateFolder(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString FolderPath;
	if (!Request.Params->TryGetStringField(TEXT("folder_path"), FolderPath))
		return InvalidParams(Request.Id, TEXT("Missing 'folder_path'"));

	auto Task = [FolderPath]() -> TSharedPtr<FJsonObject>
	{
		// Folders in UE are implicitly created by SetFolderPath on any actor.
		// Setting it on WorldSettings makes the folder visible even with no child actors.
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		if (AWorldSettings* WS = World->GetWorldSettings())
		{
			const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: create folder")));
			WS->Modify();
			WS->SetFolderPath(FName(*FolderPath));
			WS->MarkPackageDirty();
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Created folder %s"), *FolderPath);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleMoveActorToFolder(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, FolderPath;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("folder_path"), FolderPath))
		return InvalidParams(Request.Id, TEXT("Missing 'folder_path'"));

	auto Task = [ActorName, FolderPath]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: move actor to folder")));
		Actor->Modify();
		Actor->SetFolderPath(FName(*FolderPath));
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleAddActorTag(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, Tag;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("tag"), Tag))
		return InvalidParams(Request.Id, TEXT("Missing 'tag'"));

	auto Task = [ActorName, Tag]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FName TagName(*Tag);
		if (!Actor->Tags.Contains(TagName))
		{
			const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add actor tag")));
			Actor->Modify();
			Actor->Tags.Add(TagName);
			Actor->MarkPackageDirty();
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleRemoveActorTag(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName, Tag;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetStringField(TEXT("tag"), Tag))
		return InvalidParams(Request.Id, TEXT("Missing 'tag'"));

	auto Task = [ActorName, Tag]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FName TagName(*Tag);
		if (Actor->Tags.Contains(TagName))
		{
			const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: remove actor tag")));
			Actor->Modify();
			Actor->Tags.Remove(TagName);
			Actor->MarkPackageDirty();
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetObjectField(TEXT("actor"), SerializeActor(Actor));
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Spatial
// ============================================================================
FMCPResponse FWorldService::HandleMeasureDistance(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FVector A, B;
	FString NameA, NameB;
	const bool bHasA = FMCPJson::ReadVec3(Request.Params, TEXT("point_a"), A);
	const bool bHasB = FMCPJson::ReadVec3(Request.Params, TEXT("point_b"), B);
	const bool bHasNameA = Request.Params->TryGetStringField(TEXT("actor_a"), NameA);
	const bool bHasNameB = Request.Params->TryGetStringField(TEXT("actor_b"), NameB);

	if ((!bHasA && !bHasNameA) || (!bHasB && !bHasNameB))
		return InvalidParams(Request.Id, TEXT("Provide point_a+point_b or actor_a+actor_b (mix allowed)"));

	auto Task = [A, B, NameA, NameB, bHasNameA, bHasNameB]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FVector PA = A, PB = B;
		if (bHasNameA)
		{
			AActor* Actor = FMCPActorResolver::ByLabel(World, NameA);
			if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("actor_a not found: %s"), *NameA));
			PA = Actor->GetActorLocation();
		}
		if (bHasNameB)
		{
			AActor* Actor = FMCPActorResolver::ByLabel(World, NameB);
			if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("actor_b not found: %s"), *NameB));
			PB = Actor->GetActorLocation();
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("distance"), FVector::Dist(PA, PB));
		Result->SetNumberField(TEXT("distance_2d"), FVector::Dist2D(PA, PB));
		FMCPJson::WriteVec3(Result, TEXT("point_a"), PA);
		FMCPJson::WriteVec3(Result, TEXT("point_b"), PB);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleFindActorsInRadius(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FVector Center;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("center"), Center))
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'center'"));
	double Radius = 0;
	if (!Request.Params->TryGetNumberField(TEXT("radius"), Radius))
		return InvalidParams(Request.Id, TEXT("Missing 'radius'"));

	auto Task = [Center, Radius]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		TArray<TSharedPtr<FJsonValue>> ActorsJson;
		const double RadiusSq = Radius * Radius;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			const double DistSq = FVector::DistSquared(Actor->GetActorLocation(), Center);
			if (DistSq > RadiusSq) continue;
			TSharedPtr<FJsonObject> O = SerializeActor(Actor);
			O->SetNumberField(TEXT("distance"), FMath::Sqrt(DistSq));
			ActorsJson.Add(MakeShared<FJsonValueObject>(O));
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("count"), ActorsJson.Num());
		Result->SetArrayField(TEXT("actors"), ActorsJson);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleFindActorsInBounds(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FVector Min, Max;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("min"), Min))
		return InvalidParams(Request.Id, TEXT("Missing 'min' [X,Y,Z]"));
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("max"), Max))
		return InvalidParams(Request.Id, TEXT("Missing 'max' [X,Y,Z]"));

	auto Task = [Min, Max]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FBox Box(Min, Max);
		TArray<TSharedPtr<FJsonValue>> ActorsJson;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			if (Box.IsInside(Actor->GetActorLocation()))
				ActorsJson.Add(MakeShared<FJsonValueObject>(SerializeActor(Actor)));
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("count"), ActorsJson.Num());
		Result->SetArrayField(TEXT("actors"), ActorsJson);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleRaycast(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FVector Start, End;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("start"), Start))
		return InvalidParams(Request.Id, TEXT("Missing 'start'"));
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("end"), End))
		return InvalidParams(Request.Id, TEXT("Missing 'end'"));

	auto Task = [Start, End]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SpecialAgentRaycast), true);
		const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("hit"), bHit);
		if (bHit)
		{
			FMCPJson::WriteVec3(Result, TEXT("location"), Hit.Location);
			FMCPJson::WriteVec3(Result, TEXT("normal"), Hit.ImpactNormal);
			Result->SetNumberField(TEXT("distance"), Hit.Distance);
			if (AActor* HitActor = Hit.GetActor())
			{
				Result->SetStringField(TEXT("actor_name"), HitActor->GetActorLabel());
				Result->SetStringField(TEXT("actor_class"), HitActor->GetClass()->GetName());
			}
		}
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleGetGroundHeight(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	double X = 0, Y = 0;
	if (!Request.Params->TryGetNumberField(TEXT("x"), X))
		return InvalidParams(Request.Id, TEXT("Missing 'x'"));
	if (!Request.Params->TryGetNumberField(TEXT("y"), Y))
		return InvalidParams(Request.Id, TEXT("Missing 'y'"));
	double MaxZ = 100000.0;
	Request.Params->TryGetNumberField(TEXT("max_z"), MaxZ);

	auto Task = [X, Y, MaxZ]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FVector Start(X, Y, MaxZ);
		FVector End(X, Y, -MaxZ);
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SpecialAgentGround), true);
		const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("hit"), bHit);
		if (bHit)
		{
			Result->SetNumberField(TEXT("z"), Hit.Location.Z);
			FMCPJson::WriteVec3(Result, TEXT("location"), Hit.Location);
			FMCPJson::WriteVec3(Result, TEXT("normal"), Hit.ImpactNormal);
		}
		else
		{
			Result->SetStringField(TEXT("error"), TEXT("No ground hit along vertical trace"));
		}
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Pattern placement
// ============================================================================
// Common input: actor_class, base_rotation?, scale?
struct FPatternBaseParams { FString ActorClass; FRotator Rotation; FVector Scale; };

static bool ReadPatternBase(const TSharedPtr<FJsonObject>& Params, FPatternBaseParams& Out, FString& Err)
{
	if (!Params->TryGetStringField(TEXT("actor_class"), Out.ActorClass))
	{
		Err = TEXT("Missing 'actor_class'");
		return false;
	}
	Out.Rotation = FRotator::ZeroRotator;
	FMCPJson::ReadRotator(Params, TEXT("rotation"), Out.Rotation);
	Out.Scale = FVector(1, 1, 1);
	FMCPJson::ReadVec3(Params, TEXT("scale"), Out.Scale);
	return true;
}

static TSharedPtr<FJsonObject> SpawnMany(UWorld* World, const FPatternBaseParams& Base, const TArray<FVector>& Positions)
{
	TArray<TSharedPtr<FJsonValue>> Spawned;
	TArray<TSharedPtr<FJsonValue>> Errors;
	for (const FVector& P : Positions)
	{
		FString Type, Err;
		AActor* A = SpawnActorInternal(World, Base.ActorClass, P, Base.Rotation, Base.Scale, Type, Err);
		if (A) Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
		else Errors.Add(MakeShared<FJsonValueString>(Err));
	}
	TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
	Result->SetArrayField(TEXT("actors"), Spawned);
	Result->SetNumberField(TEXT("spawned"), Spawned.Num());
	Result->SetArrayField(TEXT("errors"), Errors);
	return Result;
}

FMCPResponse FWorldService::HandlePlaceInGrid(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FPatternBaseParams Base;
	FString Err;
	if (!ReadPatternBase(Request.Params, Base, Err)) return InvalidParams(Request.Id, Err);

	FVector Origin;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("origin"), Origin))
		return InvalidParams(Request.Id, TEXT("Missing 'origin'"));
	int32 CountX = 0, CountY = 0;
	if (!Request.Params->TryGetNumberField(TEXT("count_x"), CountX) || CountX <= 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'count_x'"));
	if (!Request.Params->TryGetNumberField(TEXT("count_y"), CountY) || CountY <= 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'count_y'"));
	double SpacingX = 100.0, SpacingY = 100.0;
	Request.Params->TryGetNumberField(TEXT("spacing_x"), SpacingX);
	Request.Params->TryGetNumberField(TEXT("spacing_y"), SpacingY);

	auto SendProgress = Ctx.SendProgress;
	auto Task = [Base, Origin, CountX, CountY, SpacingX, SpacingY, SendProgress]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: place in grid")));
		const int32 Total = CountX * CountY;
		TArray<TSharedPtr<FJsonValue>> Spawned;
		TArray<TSharedPtr<FJsonValue>> Errors;
		int32 SpawnIdx = 0;
		for (int32 iy = 0; iy < CountY; ++iy)
		{
			for (int32 ix = 0; ix < CountX; ++ix)
			{
				const FVector Pos = Origin + FVector(ix * SpacingX, iy * SpacingY, 0);
				FString Type, SpawnErr;
				AActor* A = SpawnActorInternal(World, Base.ActorClass, Pos, Base.Rotation, Base.Scale, Type, SpawnErr);
				if (A) Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
				else Errors.Add(MakeShared<FJsonValueString>(SpawnErr));
				++SpawnIdx;
				if (SpawnIdx % 8 == 0 || SpawnIdx == Total)
					SendProgress(SpawnIdx / (double)Total, 1.0,
						FString::Printf(TEXT("place_in_grid %d/%d"), SpawnIdx, Total));
			}
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), Spawned);
		Result->SetNumberField(TEXT("spawned"), Spawned.Num());
		Result->SetArrayField(TEXT("errors"), Errors);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandlePlaceInCircle(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FPatternBaseParams Base;
	FString Err;
	if (!ReadPatternBase(Request.Params, Base, Err)) return InvalidParams(Request.Id, Err);

	FVector Center;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("center"), Center))
		return InvalidParams(Request.Id, TEXT("Missing 'center'"));
	double Radius = 0;
	int32 Count = 0;
	if (!Request.Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'radius'"));
	if (!Request.Params->TryGetNumberField(TEXT("count"), Count) || Count <= 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'count'"));

	bool bFaceOut = false;
	Request.Params->TryGetBoolField(TEXT("face_outward"), bFaceOut);

	auto Task = [Base, Center, Radius, Count, bFaceOut]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: place in circle")));
		TArray<TSharedPtr<FJsonValue>> Spawned;
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (int32 i = 0; i < Count; ++i)
		{
			const double Angle = (2.0 * PI * i) / Count;
			const FVector Offset(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), 0);
			const FVector Pos = Center + Offset;
			const FRotator Rot = bFaceOut
				? (Offset.GetSafeNormal()).Rotation()
				: Base.Rotation;

			FString Type, Err2;
			AActor* A = SpawnActorInternal(World, Base.ActorClass, Pos, Rot, Base.Scale, Type, Err2);
			if (A) Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
			else Errors.Add(MakeShared<FJsonValueString>(Err2));
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), Spawned);
		Result->SetNumberField(TEXT("spawned"), Spawned.Num());
		Result->SetArrayField(TEXT("errors"), Errors);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandlePlaceAlongSpline(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FPatternBaseParams Base;
	FString Err;
	if (!ReadPatternBase(Request.Params, Base, Err)) return InvalidParams(Request.Id, Err);

	FString SplineActorName;
	if (!Request.Params->TryGetStringField(TEXT("spline_actor"), SplineActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'spline_actor' (actor with a USplineComponent)"));
	int32 Count = 0;
	if (!Request.Params->TryGetNumberField(TEXT("count"), Count) || Count <= 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'count'"));
	bool bAlign = false;
	Request.Params->TryGetBoolField(TEXT("align_to_spline"), bAlign);

	auto Task = [Base, SplineActorName, Count, bAlign]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		AActor* SplineActor = FMCPActorResolver::ByLabel(World, SplineActorName);
		if (!SplineActor) return FMCPJson::MakeError(FString::Printf(TEXT("Spline actor not found: %s"), *SplineActorName));

		USplineComponent* Spline = SplineActor->FindComponentByClass<USplineComponent>();
		if (!Spline) return FMCPJson::MakeError(TEXT("Referenced actor has no USplineComponent"));

		const float Length = Spline->GetSplineLength();
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: place along spline")));
		TArray<TSharedPtr<FJsonValue>> Spawned;
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Alpha = Count == 1 ? 0.0f : (static_cast<float>(i) / (Count - 1));
			const float Dist = Alpha * Length;
			const FVector Loc = Spline->GetLocationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);
			FRotator Rot = Base.Rotation;
			if (bAlign)
				Rot = Spline->GetRotationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);

			FString Type, Err2;
			AActor* A = SpawnActorInternal(World, Base.ActorClass, Loc, Rot, Base.Scale, Type, Err2);
			if (A) Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
			else Errors.Add(MakeShared<FJsonValueString>(Err2));
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), Spawned);
		Result->SetNumberField(TEXT("spawned"), Spawned.Num());
		Result->SetArrayField(TEXT("errors"), Errors);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleScatterInArea(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FPatternBaseParams Base;
	FString Err;
	if (!ReadPatternBase(Request.Params, Base, Err)) return InvalidParams(Request.Id, Err);

	FVector Min, Max;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("min"), Min))
		return InvalidParams(Request.Id, TEXT("Missing 'min'"));
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("max"), Max))
		return InvalidParams(Request.Id, TEXT("Missing 'max'"));
	int32 Count = 0;
	if (!Request.Params->TryGetNumberField(TEXT("count"), Count) || Count <= 0)
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'count'"));
	int32 Seed = 0;
	Request.Params->TryGetNumberField(TEXT("seed"), Seed);
	bool bStickToGround = false;
	Request.Params->TryGetBoolField(TEXT("stick_to_ground"), bStickToGround);

	auto SendProgress = Ctx.SendProgress;
	auto Task = [Base, Min, Max, Count, Seed, bStickToGround, SendProgress]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FRandomStream Rand(Seed == 0 ? FMath::RandRange(1, 1000000) : Seed);
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: scatter in area")));
		TArray<TSharedPtr<FJsonValue>> Spawned;
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (int32 i = 0; i < Count; ++i)
		{
			FVector P(
				Rand.FRandRange(static_cast<float>(Min.X), static_cast<float>(Max.X)),
				Rand.FRandRange(static_cast<float>(Min.Y), static_cast<float>(Max.Y)),
				Rand.FRandRange(static_cast<float>(Min.Z), static_cast<float>(Max.Z)));

			if (bStickToGround)
			{
				FHitResult Hit;
				FCollisionQueryParams QP(SCENE_QUERY_STAT(SpecialAgentScatter), true);
				if (World->LineTraceSingleByChannel(Hit, FVector(P.X, P.Y, Max.Z + 100.0), FVector(P.X, P.Y, Min.Z - 100.0), ECC_Visibility, QP))
					P = Hit.Location;
			}
			FString Type, SpawnErr;
			AActor* A = SpawnActorInternal(World, Base.ActorClass, P, Base.Rotation, Base.Scale, Type, SpawnErr);
			if (A) Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
			else Errors.Add(MakeShared<FJsonValueString>(SpawnErr));
			if ((i + 1) % 8 == 0 || (i + 1) == Count)
				SendProgress((i + 1.0) / (double)Count, 1.0,
					FString::Printf(TEXT("scatter_in_area %d/%d"), i + 1, Count));
		}
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("actors"), Spawned);
		Result->SetNumberField(TEXT("spawned"), Spawned.Num());
		Result->SetArrayField(TEXT("errors"), Errors);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Actor state (new tools)
// ============================================================================
FMCPResponse FWorldService::HandleSetActorTickEnabled(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	bool bEnabled = true;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		return InvalidParams(Request.Id, TEXT("Missing 'enabled'"));

	auto Task = [ActorName, bEnabled]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor tick enabled")));
		Actor->Modify();
		Actor->SetActorTickEnabled(bEnabled);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorHidden(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	bool bHidden = true;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetBoolField(TEXT("hidden"), bHidden))
		return InvalidParams(Request.Id, TEXT("Missing 'hidden'"));

	auto Task = [ActorName, bHidden]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor hidden")));
		Actor->Modify();
		Actor->SetActorHiddenInGame(bHidden);
		Actor->SetIsTemporarilyHiddenInEditor(bHidden);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetBoolField(TEXT("hidden"), bHidden);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleSetActorCollision(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	bool bEnabled = true;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	if (!Request.Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		return InvalidParams(Request.Id, TEXT("Missing 'enabled'"));

	auto Task = [ActorName, bEnabled]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set actor collision")));
		Actor->Modify();
		Actor->SetActorEnableCollision(bEnabled);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleAttachTo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ChildName, ParentName;
	if (!Request.Params->TryGetStringField(TEXT("child"), ChildName))
		return InvalidParams(Request.Id, TEXT("Missing 'child'"));
	if (!Request.Params->TryGetStringField(TEXT("parent"), ParentName))
		return InvalidParams(Request.Id, TEXT("Missing 'parent'"));
	FString SocketName;
	Request.Params->TryGetStringField(TEXT("socket"), SocketName);
	FString RuleStr = TEXT("keep_world");
	Request.Params->TryGetStringField(TEXT("rule"), RuleStr);

	auto Task = [ChildName, ParentName, SocketName, RuleStr]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Child = FMCPActorResolver::ByLabel(World, ChildName);
		AActor* Parent = FMCPActorResolver::ByLabel(World, ParentName);
		if (!Child) return FMCPJson::MakeError(FString::Printf(TEXT("Child not found: %s"), *ChildName));
		if (!Parent) return FMCPJson::MakeError(FString::Printf(TEXT("Parent not found: %s"), *ParentName));

		FAttachmentTransformRules Rules = FAttachmentTransformRules::KeepWorldTransform;
		if (RuleStr.Equals(TEXT("keep_relative"), ESearchCase::IgnoreCase))
			Rules = FAttachmentTransformRules::KeepRelativeTransform;
		else if (RuleStr.Equals(TEXT("snap_to_target"), ESearchCase::IgnoreCase))
			Rules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: attach actor")));
		Child->Modify();
		Child->AttachToActor(Parent, Rules, FName(*SocketName));
		Child->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("child"), ChildName);
		Result->SetStringField(TEXT("parent"), ParentName);
		Result->SetStringField(TEXT("rule"), RuleStr);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Attached %s -> %s"), *ChildName, *ParentName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleDetach(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

	auto Task = [ActorName]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: detach actor")));
		Actor->Modify();
		Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		Actor->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Detached %s"), *ActorName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// ============================================================================
// Tool schemas
// ============================================================================
TArray<FMCPToolInfo> FWorldService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	// ---------- Queries ----------
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_actors");
		Tool.Description = TEXT("List actors in the active editor world (editor-world only; excludes PIE actors). "
			"Returns {actors:[{name,class,location,rotation,scale,tags}], count}; 'name' is the outliner label that feeds get_actor / set_actor_* / delete_actor. "
			"Params: class_filter (string, optional case-sensitive substring matched against the class name, e.g. \"StaticMeshActor\"; default empty = all), "
			"max_results (integer, optional, default 1000). "
			"Workflow: call before spawn/delete/transform tools to discover the exact labels to pass downstream. "
			"Warning: read-only; iterates every actor, so cap large levels with max_results. Substring match, not a class path.");
		TSharedPtr<FJsonObject> ClassParam = MakeShared<FJsonObject>();
		ClassParam->SetStringField(TEXT("type"), TEXT("string"));
		ClassParam->SetStringField(TEXT("description"), TEXT("Case-sensitive substring matched against the actor's class name (e.g. \"StaticMeshActor\"). Empty = all actors."));
		Tool.Parameters->SetObjectField(TEXT("class_filter"), ClassParam);
		TSharedPtr<FJsonObject> MaxParam = MakeShared<FJsonObject>();
		MaxParam->SetStringField(TEXT("type"), TEXT("integer"));
		MaxParam->SetStringField(TEXT("description"), TEXT("Max actors to return (default 1000)."));
		MaxParam->SetNumberField(TEXT("default"), 1000);
		Tool.Parameters->SetObjectField(TEXT("max_results"), MaxParam);
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("get_actor"),
		TEXT("Look up one actor by its outliner label and return its current transform. "
			 "Returns {actor:{name,class,location[cm],rotation[deg],scale,tags}}, or an error if no actor has that label. "
			 "Params: actor_name (string, required, exact outliner label as shown by list_actors). "
			 "Workflow: list_actors to find the label, then get_actor to inspect, then set_actor_* to modify. "
			 "Warning: read-only; resolves by label, so duplicate labels resolve to the first match."))
		.RequiredString(TEXT("actor_name"), TEXT("Exact outliner label of the actor to look up"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("find_actors_by_tag"),
		TEXT("Find every actor that carries the given actor tag (the FName entries in Actor.Tags, not component tags). "
			 "Returns {actors:[{name,class,location,rotation,scale,tags}], count}. "
			 "Params: tag (string, required, exact tag name; case-sensitive). "
			 "Workflow: tag actors with add_actor_tag, then gather them here and feed the labels to delete_actors_batch or transform tools. "
			 "Warning: read-only; matches the whole tag exactly, not a substring."))
		.RequiredString(TEXT("tag"), TEXT("Exact actor tag to search for (case-sensitive)"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_level_info");
		Tool.Description = TEXT("Summarize the active editor world: map name, package path, total actor count, and combined world-space bounds. "
			"Returns {level_name, level_path, actor_count, bounds:{min,max,center,size}} (all in cm; bounds omitted if the level is empty). "
			"Params: (none). "
			"Workflow: call first when exploring an unfamiliar scene to learn its extents before placing or querying actors. "
			"Warning: read-only; bounds are derived from every actor's component bounding box (excludes WorldSettings) and can be large.");
		Tools.Add(Tool);
	}

	// ---------- Spawn / Delete ----------
	Tools.Add(FMCPToolBuilder(TEXT("spawn_actor"),
		TEXT("Spawn a single actor into the current editor level at a world-space location. "
			 "actor_class accepts a StaticMesh asset path (wrapped in a StaticMeshActor), a Blueprint asset path (its generated class), or a bare native class name; "
			 "returns {spawned_type:\"StaticMesh\"|\"Blueprint\"|\"Class\", actor:{name,class,location,rotation,scale,tags}}. "
			 "Params: actor_class (string, required; full asset path like /Game/Meshes/Rock.Rock for meshes/blueprints, or a class name like \"PointLight\"), "
			 "location (array [X,Y,Z] world-space cm, required), rotation (array [Pitch,Yaw,Roll] degrees, optional default [0,0,0]), "
			 "scale (array [X,Y,Z] unitless, optional default [1,1,1]). "
			 "Workflow: for many actors use spawn_actors_batch / place_in_grid instead; use get_ground_height or raycast to project onto terrain first. "
			 "Warning: changes the level in memory only (not saved to disk); location is the actor pivot, not the visual center. Returns an error if the path/class cannot be resolved."))
		.RequiredString(TEXT("actor_class"), TEXT("StaticMesh/Blueprint asset path (/Game/...) or a native class name (e.g. PointLight)"))
		.RequiredVec3(TEXT("location"), TEXT("World-space spawn location [X, Y, Z] in cm"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X, Y, Z], unitless (default [1,1,1])"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("spawn_actors_batch");
		Tool.Description = TEXT("Spawn many actors in one round-trip, reporting per-entry successes and failures. "
			"Returns {actors:[serialized spawned actors], spawned (count), errors:[strings]}. "
			"Params: spawns (array, required; each entry {actor_class:string (asset path or class name), location:[X,Y,Z] cm, rotation?:[Pitch,Yaw,Roll] deg default [0,0,0], scale?:[X,Y,Z] default [1,1,1]}). "
			"Entries missing actor_class or location are skipped silently. "
			"Workflow: prefer over looping spawn_actor; for regular layouts use place_in_grid / place_in_circle, for vegetation use foliage tools. "
			"Warning: partial success is normal -- always inspect the 'errors' array. Streams progress; changes are in-memory only (not saved).");
		TSharedPtr<FJsonObject> SpawnsParam = MakeShared<FJsonObject>();
		SpawnsParam->SetStringField(TEXT("type"), TEXT("array"));
		SpawnsParam->SetStringField(TEXT("description"), TEXT("Array of spawn specs: [{actor_class, location:[X,Y,Z] cm, rotation?:[Pitch,Yaw,Roll] deg, scale?:[X,Y,Z]}, ...]. Entries without actor_class or location are skipped."));
		Tool.Parameters->SetObjectField(TEXT("spawns"), SpawnsParam);
		Tool.RequiredParams.Add(TEXT("spawns"));
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("delete_actor"),
		TEXT("Destroy one actor in the editor world, resolved by outliner label. "
			 "Returns {actor_name} on success, or an error if no actor has that label. "
			 "Params: actor_name (string, required, exact outliner label). "
			 "Workflow: list_actors or find_actors_by_tag to gather candidates first; for many actors use delete_actors_batch. "
			 "Warning: destructive -- the actor is removed immediately, but the delete is wrapped in an undo transaction (Ctrl+Z restores it). Change is in-memory; save the level to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Exact outliner label of the actor to destroy"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("delete_actors_batch"),
		TEXT("Destroy many actors in one call, resolving each by outliner label. "
			 "Returns {deleted (count), not_found:[labels that matched no actor]}. "
			 "Params: actor_names (array of strings, required; exact outliner labels). "
			 "Workflow: gather labels via find_actors_by_tag / find_actors_in_radius / find_actors_in_bounds, then pass them here. "
			 "Warning: destructive but undoable (wrapped in a single undo transaction). Streams progress. Change is in-memory; save the level to persist."))
		.RequiredArrayOfString(TEXT("actor_names"), TEXT("Exact outliner labels of the actors to destroy"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("duplicate_actor"),
		TEXT("Duplicate an existing actor via the editor copy/paste path and return the new actor. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}} -- the duplicate gets an auto-generated unique label. "
			 "Params: actor_name (string, required, source actor's outliner label), "
			 "new_location (array [X,Y,Z] world-space cm, optional; an ABSOLUTE location for the copy, not an offset -- omit to leave it at the source position). "
			 "Workflow: follow with set_actor_label to rename the copy, and set_actor_location/transform if you skipped new_location. "
			 "Warning: the editor selection is captured before and restored after the duplicate, so your prior selection is preserved; change is in-memory only. Complex components may not deep-copy reliably -- verify the result."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor to duplicate"))
		.OptionalVec3(TEXT("new_location"), TEXT("Absolute world-space location [X,Y,Z] cm for the duplicate (default: same as source)"))
		.Build());

	// ---------- Transforms ----------
	Tools.Add(FMCPToolBuilder(TEXT("set_actor_transform"),
		TEXT("Set any combination of world-space location, rotation, and scale on one actor in a single call; omitted components keep their current value. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}}. Provide at least one of location/rotation/scale or it errors. "
			 "Params: actor_name (string, required, outliner label), location (array [X,Y,Z] world-space cm, optional), "
			 "rotation (array [Pitch,Yaw,Roll] degrees, optional; note the Details panel labels these axes Roll/Pitch/Yaw), scale (array [X,Y,Z] unitless, optional). "
			 "Workflow: prefer this over separate set_actor_location/rotation/scale calls to avoid intermediate states. "
			 "Warning: in-memory only; save the level to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.OptionalVec3(TEXT("location"), TEXT("World-space location [X,Y,Z] in cm"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch,Yaw,Roll] in degrees"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z], unitless"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_location"),
		TEXT("Move an actor to a new world-space location, leaving its rotation and scale unchanged. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}}. "
			 "Params: actor_name (string, required, outliner label), location (array [X,Y,Z] world-space cm, required). "
			 "Workflow: use set_actor_transform instead when also changing rotation/scale to apply them atomically. "
			 "Warning: in-memory only; save the level to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredVec3(TEXT("location"), TEXT("New world-space location [X, Y, Z] in cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_rotation"),
		TEXT("Set an actor's world-space rotation, leaving location and scale unchanged. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}}. "
			 "Params: actor_name (string, required, outliner label), rotation (array [Pitch,Yaw,Roll] degrees, required; "
			 "Rotator order is pitch,yaw,roll even though the Details panel labels the axes Roll/Pitch/Yaw). "
			 "Workflow: pair with raycast/get_ground_height -- use the returned surface normal to align actors to terrain. "
			 "Warning: in-memory only; save the level to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredVec3(TEXT("rotation"), TEXT("Rotation [Pitch, Yaw, Roll] in degrees"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_scale"),
		TEXT("Set an actor's 3D scale, leaving location and rotation unchanged. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}}. "
			 "Params: actor_name (string, required, outliner label), scale (array [X,Y,Z] unitless where 1 = original size, required). "
			 "Workflow: query assets/get_bounds first to pick a scale matching surrounding objects. "
			 "Warning: in-memory only; save the level to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredVec3(TEXT("scale"), TEXT("Scale [X, Y, Z], unitless (1 = original)"))
		.Build());

	// ---------- Properties ----------
	Tools.Add(FMCPToolBuilder(TEXT("set_actor_property"),
		TEXT("Set any reflected UProperty on an actor by importing a string value via FProperty::ImportText_Direct, then PostEditChange. "
			 "Returns {property, value}, or an error if the property name is not found on the class or the value cannot be parsed. "
			 "Params: actor_name (string, required, outliner label), property_name (string, required, exact reflected UProperty name -- C++ name, not the Details-panel display name), "
			 "value (string, required, UE text serialization of the value; e.g. \"100\", \"true\", or \"(X=1,Y=2,Z=3)\" for a struct). "
			 "Workflow: discover available property names via reflection/inspect tools before calling. "
			 "Warning: only reflected UProperties work; struct values need parenthesized tuple syntax. In-memory only; save to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredString(TEXT("property_name"), TEXT("Exact reflected UProperty name (C++ name, not the display name)"))
		.RequiredString(TEXT("value"), TEXT("UE text-serialized value (e.g. \"100\", \"true\", \"(X=1,Y=2,Z=3)\")"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_label"),
		TEXT("Rename an actor's outliner display label. "
			 "Returns {old_label, new_label} -- read back new_label, since UE may have adjusted it to keep labels unique. "
			 "Params: actor_name (string, required, current outliner label), new_label (string, required, desired label). "
			 "Workflow: use after duplicate_actor / spawn_actor to give new actors meaningful names that later tools can resolve by label. "
			 "Warning: labels should be unique; UE may append a numeric suffix on collision, so use the returned new_label for subsequent calls."))
		.RequiredString(TEXT("actor_name"), TEXT("Current outliner label of the actor"))
		.RequiredString(TEXT("new_label"), TEXT("Desired new label"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_material"),
		TEXT("Assign a material asset to the given slot on every MeshComponent of an actor (resolved by label). "
			 "Returns {components_updated (count), slot_index, material_path}, or an error if the actor has no MeshComponent or the material fails to load. "
			 "Params: actor_name (string, required, outliner label), material_path (string, required, full asset object path like /Game/Mats/M_Wood.M_Wood), "
			 "slot_index (integer, optional, default 0, the material element index). "
			 "Workflow: create a per-actor MaterialInstanceConstant via material tools, then assign it here; use set_material_parameter for non-destructive tweaks instead. "
			 "Warning: overrides the slot on every mesh component of the actor; in-memory only, save the level to persist."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredString(TEXT("material_path"), TEXT("Material asset object path, e.g. /Game/Mats/M_Wood.M_Wood"))
		.OptionalInteger(TEXT("slot_index"), TEXT("Material element/slot index (default 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_material_parameter"),
		TEXT("Set a scalar or vector parameter on an actor's current slot material, persisting to the asset when the slot holds a MaterialInstanceConstant. "
			 "Returns {components_updated, persisted (bool), note?}: if the slot material is a MaterialInstanceConstant the value is written persistently via its EditorOnly setter (PostEditChange + MarkPackageDirty) and persisted=true; otherwise it falls back to a transient MaterialInstanceDynamic and persisted=false with a note pointing to material/* tools. A missing parameter name does NOT error, so verify visually. "
			 "Params: actor_name (string, required, outliner label), parameter_name (string, required, exact parameter name on the base material), "
			 "parameter_type (enum, required: \"scalar\" or \"vector\"), value (required: a number when parameter_type=\"scalar\"; "
			 "a linear-color array [R,G,B,A] in linear 0..1 (may exceed 1 for HDR) when parameter_type=\"vector\"), slot_index (integer, optional, default 0). "
			 "Workflow: assign a MaterialInstanceConstant via set_actor_material or material/* tools first to get persisted=true; with a plain base material this only swaps in a session-only dynamic instance. "
			 "Warning: when persisted=false the change lives only in this editor session and is never saved to disk -- edit a MaterialInstanceConstant via the material/* tools for a durable asset edit. When persisted=true the owning MIC package is marked dirty and must be saved to write it to disk."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredString(TEXT("parameter_name"), TEXT("Exact parameter name declared on the base material"))
		.RequiredEnum(TEXT("parameter_type"), {TEXT("scalar"), TEXT("vector")}, TEXT("\"scalar\" for a float value, \"vector\" for a [R,G,B,A] linear color"))
		.RequiredAny(TEXT("value"), TEXT("Number when parameter_type=scalar; [R,G,B,A] linear 0..1 (HDR may exceed 1) when parameter_type=vector"))
		.OptionalInteger(TEXT("slot_index"), TEXT("Material element/slot index (default 0)"))
		.Build());

	// ---------- Organization ----------
	Tools.Add(FMCPToolBuilder(TEXT("create_folder"),
		TEXT("Make an outliner folder visible by setting the folder path on the level's WorldSettings actor. "
			 "Returns {folder_path}. The folder shows even with no child actors. "
			 "Params: folder_path (string, required; use forward slashes for nesting, e.g. \"Lighting/Sun\"). "
			 "Workflow: create the folder, then move_actor_to_folder to populate it. "
			 "Warning: in-memory until the level is saved; reassigns WorldSettings' own folder path each call (not destructive to actors)."))
		.RequiredString(TEXT("folder_path"), TEXT("Outliner folder path, slashes for nesting (e.g. \"Lighting/Sun\")"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("move_actor_to_folder"),
		TEXT("Move an actor into an outliner folder (creating the folder implicitly if it does not yet exist). "
			 "Returns {actor:{...}, folder_path}. "
			 "Params: actor_name (string, required, outliner label), folder_path (string, required; forward slashes for nesting). "
			 "Workflow: organizational only -- pair with list_actors to confirm placement; create_folder is optional since the folder is created on demand. "
			 "Warning: in-memory until the level is saved."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredString(TEXT("folder_path"), TEXT("Target outliner folder path, slashes for nesting"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("add_actor_tag"),
		TEXT("Add an actor tag (an FName in Actor.Tags) if it is not already present. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}}. Idempotent -- adding an existing tag is a no-op. "
			 "Params: actor_name (string, required, outliner label), tag (string, required, exact tag name). "
			 "Workflow: tag actors here, then gather them later with find_actors_by_tag. "
			 "Warning: in-memory until the level is saved; this sets the actor's own tags, not component tags."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredString(TEXT("tag"), TEXT("Exact actor tag to add"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("remove_actor_tag"),
		TEXT("Remove an actor tag (an FName in Actor.Tags) if present. "
			 "Returns {actor:{name,class,location,rotation,scale,tags}}. No-op if the tag is absent. "
			 "Params: actor_name (string, required, outliner label), tag (string, required, exact tag name). "
			 "Workflow: pair with find_actors_by_tag to confirm the tag was present beforehand. "
			 "Warning: in-memory until the level is saved."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredString(TEXT("tag"), TEXT("Exact actor tag to remove"))
		.Build());

	// ---------- Spatial ----------
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("measure_distance");
		Tool.Description = TEXT("Measure the straight-line and ground-plane distance between two endpoints, each given as a raw point OR an actor (the two styles can be mixed). "
			"Returns {distance (3D, cm), distance_2d (XY only, cm), point_a, point_b} -- point_* echo the resolved world positions. "
			"Params: point_a / point_b (array [X,Y,Z] world-space cm) OR actor_a / actor_b (string outliner label, uses the actor's location); "
			"supply one of {point_a, actor_a} and one of {point_b, actor_b}. "
			"Workflow: combine with find_actors_in_radius / find_actors_in_bounds for spatial reasoning. "
			"Warning: read-only; an actor endpoint that resolves no actor returns an error.");
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("array"));
		P->SetStringField(TEXT("description"), TEXT("Endpoint A as world-space point [X,Y,Z] in cm (alternative to actor_a)"));
		Tool.Parameters->SetObjectField(TEXT("point_a"), P);
		TSharedPtr<FJsonObject> P2 = MakeShared<FJsonObject>();
		P2->SetStringField(TEXT("type"), TEXT("array"));
		P2->SetStringField(TEXT("description"), TEXT("Endpoint B as world-space point [X,Y,Z] in cm (alternative to actor_b)"));
		Tool.Parameters->SetObjectField(TEXT("point_b"), P2);
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("type"), TEXT("string"));
		A->SetStringField(TEXT("description"), TEXT("Endpoint A as an actor outliner label (alternative to point_a)"));
		Tool.Parameters->SetObjectField(TEXT("actor_a"), A);
		TSharedPtr<FJsonObject> A2 = MakeShared<FJsonObject>();
		A2->SetStringField(TEXT("type"), TEXT("string"));
		A2->SetStringField(TEXT("description"), TEXT("Endpoint B as an actor outliner label (alternative to point_b)"));
		Tool.Parameters->SetObjectField(TEXT("actor_b"), A2);
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("find_actors_in_radius"),
		TEXT("Find every actor whose pivot location lies within a sphere of the given radius around a center point. "
			 "Returns {count, actors:[{name,class,location,...,distance (cm)}]} sorted in iteration order, each annotated with its distance from center. "
			 "Params: center (array [X,Y,Z] world-space cm, required), radius (number cm, required). "
			 "Workflow: feed the returned labels to delete_actors_batch or transform tools for spatial cleanup. "
			 "Warning: read-only; tests the actor pivot only, not its bounds -- a large actor may be missed if its pivot is outside the sphere."))
		.RequiredVec3(TEXT("center"), TEXT("Sphere center [X,Y,Z] in cm"))
		.RequiredNumber(TEXT("radius"), TEXT("Sphere radius in cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("find_actors_in_bounds"),
		TEXT("Find every actor whose pivot location lies inside an axis-aligned bounding box. "
			 "Returns {count, actors:[{name,class,location,...}]}. "
			 "Params: min (array [X,Y,Z] cm, required, box minimum corner), max (array [X,Y,Z] cm, required, box maximum corner). "
			 "Workflow: cheaper than find_actors_in_radius for rectangular regions; feed labels to bulk delete/transform tools. "
			 "Warning: read-only; tests the actor pivot only, not its bounds."))
		.RequiredVec3(TEXT("min"), TEXT("Box minimum corner [X,Y,Z] in cm"))
		.RequiredVec3(TEXT("max"), TEXT("Box maximum corner [X,Y,Z] in cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("raycast"),
		TEXT("Single line-trace from start to end on the Visibility collision channel. "
			 "Returns {hit (bool)} plus, when hit, {location[cm], normal (impact normal), distance[cm], actor_name, actor_class}. "
			 "Params: start (array [X,Y,Z] world-space cm, required), end (array [X,Y,Z] world-space cm, required). "
			 "Workflow: use to test line-of-sight or find a precise surface point/normal; get_ground_height is the vertical-drop convenience wrapper. "
			 "Warning: read-only; only hits actors with collision enabled on the Visibility channel -- objects with collision disabled are invisible to the trace."))
		.RequiredVec3(TEXT("start"), TEXT("Trace start [X,Y,Z] in cm"))
		.RequiredVec3(TEXT("end"), TEXT("Trace end [X,Y,Z] in cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("get_ground_height"),
		TEXT("Find ground elevation at an (x,y) column by tracing straight down on the Visibility channel from +max_z to -max_z. "
			 "Returns {hit (bool), z (ground Z, cm), location, normal} on success; on a miss returns {hit:false, error}. "
			 "Params: x (number, world X in cm, required), y (number, world Y in cm, required), max_z (number cm, optional, default 100000 -- the half-height of the vertical trace). "
			 "Workflow: pair with spawn_actor / scatter_in_area to drop props onto terrain. "
			 "Warning: read-only; requires collision on the Visibility channel under (x,y) or it reports no hit."))
		.RequiredNumber(TEXT("x"), TEXT("World X coordinate in cm"))
		.RequiredNumber(TEXT("y"), TEXT("World Y coordinate in cm"))
		.OptionalNumber(TEXT("max_z"), TEXT("Half-height of the vertical trace in cm (default 100000)"))
		.Build());

	// ---------- Pattern placement ----------
	Tools.Add(FMCPToolBuilder(TEXT("place_in_grid"),
		TEXT("Spawn a rectangular grid of identical actors: count_x by count_y instances stepped by spacing along +X and +Y from an origin. "
			 "Returns {actors:[spawned], spawned (count), errors:[strings]}. "
			 "Params: actor_class (string, required, asset path or class name), origin (array [X,Y,Z] world-space cm, required, corner of the grid), "
			 "count_x (integer >0, required, columns along +X), count_y (integer >0, required, rows along +Y), "
			 "spacing_x (number cm, optional, default 100), spacing_y (number cm, optional, default 100), "
			 "rotation (array [Pitch,Yaw,Roll] deg, optional, applied to every instance), scale (array [X,Y,Z], optional, default [1,1,1]). "
			 "Workflow: prefer over looping spawn_actor for regular layouts; for organic vegetation use foliage tools instead. "
			 "Warning: count_x*count_y can be huge with no automatic cap -- review before calling. Streams progress; in-memory only."))
		.RequiredString(TEXT("actor_class"), TEXT("StaticMesh/Blueprint asset path or native class name"))
		.RequiredVec3(TEXT("origin"), TEXT("Grid corner origin [X,Y,Z] in cm"))
		.RequiredInteger(TEXT("count_x"), TEXT("Number of columns along +X (>0)"))
		.RequiredInteger(TEXT("count_y"), TEXT("Number of rows along +Y (>0)"))
		.OptionalNumber(TEXT("spacing_x"), TEXT("Column spacing along +X in cm (default 100)"))
		.OptionalNumber(TEXT("spacing_y"), TEXT("Row spacing along +Y in cm (default 100)"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch,Yaw,Roll] deg applied to every instance"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z] (default [1,1,1])"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("place_along_spline"),
		TEXT("Spawn N actors evenly spaced by arc length along an existing spline actor's USplineComponent. "
			 "Returns {actors:[spawned], spawned (count), errors:[strings]}. "
			 "Params: actor_class (string, required, asset path or class name), spline_actor (string, required, outliner label of an actor that owns a USplineComponent), "
			 "count (integer >0, required), align_to_spline (bool, optional, default false -- when true each actor is oriented to the spline tangent and rotation is ignored), "
			 "rotation (array [Pitch,Yaw,Roll] deg, optional, used only when align_to_spline is false), scale (array [X,Y,Z], optional, default [1,1,1]). "
			 "Workflow: spawn or pick a spline actor first; this samples world-space positions along its length. "
			 "Warning: errors if spline_actor has no USplineComponent. In-memory only."))
		.RequiredString(TEXT("actor_class"), TEXT("StaticMesh/Blueprint asset path or native class name"))
		.RequiredString(TEXT("spline_actor"), TEXT("Outliner label of an actor that owns a USplineComponent"))
		.RequiredInteger(TEXT("count"), TEXT("Number of actors to spawn (>0)"))
		.OptionalBool(TEXT("align_to_spline"), TEXT("Orient each actor to the spline tangent (overrides rotation)"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch,Yaw,Roll] deg, used only when align_to_spline=false"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z] (default [1,1,1])"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("place_in_circle"),
		TEXT("Spawn N actors evenly distributed around a horizontal circle (in the XY plane at the center's Z). "
			 "Returns {actors:[spawned], spawned (count), errors:[strings]}. "
			 "Params: actor_class (string, required, asset path or class name), center (array [X,Y,Z] world-space cm, required), "
			 "radius (number cm >0, required), count (integer >0, required), face_outward (bool, optional, default false -- when true each actor faces radially away from center and rotation is ignored), "
			 "rotation (array [Pitch,Yaw,Roll] deg, optional, used only when face_outward is false), scale (array [X,Y,Z], optional, default [1,1,1]). "
			 "Workflow: pair with get_ground_height afterward to project each spawn onto terrain if needed. "
			 "Warning: in-memory only; circle is flat at center Z, not draped over terrain."))
		.RequiredString(TEXT("actor_class"), TEXT("StaticMesh/Blueprint asset path or native class name"))
		.RequiredVec3(TEXT("center"), TEXT("Circle center [X,Y,Z] in cm"))
		.RequiredNumber(TEXT("radius"), TEXT("Circle radius in cm (>0)"))
		.RequiredInteger(TEXT("count"), TEXT("Number of actors (>0)"))
		.OptionalBool(TEXT("face_outward"), TEXT("Rotate each actor to face away from center (overrides rotation)"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch,Yaw,Roll] deg, used only when face_outward=false"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z] (default [1,1,1])"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("scatter_in_area"),
		TEXT("Randomly scatter N actors inside an axis-aligned box using a deterministic seeded RNG, optionally snapping each to the ground. "
			 "Returns {actors:[spawned], spawned (count), errors:[strings]}. "
			 "Params: actor_class (string, required, asset path or class name), min (array [X,Y,Z] cm, required, box minimum corner), max (array [X,Y,Z] cm, required, box maximum corner), "
			 "count (integer >0, required), seed (integer, optional; 0 = pick a random seed each run, any non-zero value = reproducible), "
			 "stick_to_ground (bool, optional, default false -- raycasts down to drop each actor onto collision), rotation (array [Pitch,Yaw,Roll] deg, optional), scale (array [X,Y,Z], optional, default [1,1,1]). "
			 "Workflow: pass a non-zero seed for repeatable layouts; for dense vegetation use foliage tools instead. "
			 "Warning: stick_to_ground raycasts per actor -- slow for large counts. Streams progress; in-memory only."))
		.RequiredString(TEXT("actor_class"), TEXT("StaticMesh/Blueprint asset path or native class name"))
		.RequiredVec3(TEXT("min"), TEXT("Box minimum corner [X,Y,Z] in cm"))
		.RequiredVec3(TEXT("max"), TEXT("Box maximum corner [X,Y,Z] in cm"))
		.RequiredInteger(TEXT("count"), TEXT("Number of actors to scatter (>0)"))
		.OptionalInteger(TEXT("seed"), TEXT("RNG seed; 0 = random each run, non-zero = reproducible"))
		.OptionalBool(TEXT("stick_to_ground"), TEXT("Raycast down to snap each actor onto collision"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch,Yaw,Roll] deg applied to every instance"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z] (default [1,1,1])"))
		.Build());

	// ---------- Actor state ----------
	Tools.Add(FMCPToolBuilder(TEXT("set_actor_tick_enabled"),
		TEXT("Enable or disable an actor's per-frame Tick. "
			 "Returns {actor_name, enabled}. "
			 "Params: actor_name (string, required, outliner label), enabled (bool, required; true = tick on, false = tick off). "
			 "Workflow: editor-world actors do not tick anyway, so this mainly matters for PIE/runtime behavior. "
			 "Warning: in-memory state change; not saved unless the actor's package is saved."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredBool(TEXT("enabled"), TEXT("true = tick on, false = tick off"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_hidden"),
		TEXT("Hide or show an actor in BOTH the editor viewport and at runtime (sets SetActorHiddenInGame and SetIsTemporarilyHiddenInEditor together). "
			 "Returns {actor_name, hidden}. "
			 "Params: actor_name (string, required, outliner label), hidden (bool, required; true = hide, false = show). "
			 "Workflow: hide clutter before a screenshot, then show it again afterward. "
			 "Warning: in-memory; editor-hidden state is not persisted with the level."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredBool(TEXT("hidden"), TEXT("true = hide, false = show"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_collision"),
		TEXT("Enable or disable all collision on an actor (Actor::SetActorEnableCollision). "
			 "Returns {actor_name, enabled}. "
			 "Params: actor_name (string, required, outliner label), enabled (bool, required; true = enable, false = disable). "
			 "Workflow: temporarily disable collision when an actor blocks raycast/get_ground_height during placement, then re-enable it. "
			 "Warning: a disabled actor becomes invisible to raycast and get_ground_height. In-memory state change."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor"))
		.RequiredBool(TEXT("enabled"), TEXT("true = enable collision, false = disable"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("attach_to"),
		TEXT("Attach one actor (child) to another (parent) so the child follows the parent's transform. "
			 "Returns {child, parent, rule}. "
			 "Params: child (string, required, outliner label), parent (string, required, outliner label), "
			 "socket (string, optional, socket/bone name on the parent's root component; empty = root), "
			 "rule (enum, optional, default \"keep_world\": \"keep_world\" preserves world transform, \"keep_relative\" keeps current relative transform, \"snap_to_target\" snaps to the socket excluding scale). "
			 "Workflow: use detach to reverse. "
			 "Warning: mutates the scene hierarchy in-memory; pick the rule deliberately since it determines whether the child visually moves."))
		.RequiredString(TEXT("child"), TEXT("Outliner label of the child actor"))
		.RequiredString(TEXT("parent"), TEXT("Outliner label of the parent actor"))
		.OptionalString(TEXT("socket"), TEXT("Socket/bone name on the parent's root component (empty = root)"))
		.OptionalEnum(TEXT("rule"), {TEXT("keep_world"), TEXT("keep_relative"), TEXT("snap_to_target")},
			TEXT("Attachment transform rule (default keep_world)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("detach"),
		TEXT("Detach an actor from its current parent, preserving its world transform. "
			 "Returns {actor_name}. "
			 "Params: actor_name (string, required, outliner label of the child to detach). "
			 "Workflow: the inverse of attach_to. "
			 "Warning: no-op if the actor is not attached; in-memory hierarchy change."))
		.RequiredString(TEXT("actor_name"), TEXT("Outliner label of the actor to detach"))
		.Build());

	return Tools;
}

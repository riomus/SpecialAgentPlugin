// Copyright Epic Games, Inc. All Rights Reserved.
// WorldService Implementation - Core world/actor manipulation methods

#include "Services/WorldService.h"
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
	return NewActor;
}

// ============================================================================
// Request Router
// ============================================================================
FMCPResponse FWorldService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	// Query methods
	if (MethodName == TEXT("list_actors")) return HandleListActors(Request);
	if (MethodName == TEXT("get_actor")) return HandleGetActor(Request);
	if (MethodName == TEXT("find_actors_by_tag")) return HandleFindActorsByTag(Request);
	if (MethodName == TEXT("get_level_info")) return HandleGetLevelInfo(Request);

	// Spawn/Delete methods
	if (MethodName == TEXT("spawn_actor")) return HandleSpawnActor(Request);
	if (MethodName == TEXT("spawn_actors_batch")) return HandleSpawnActorsBatch(Request);
	if (MethodName == TEXT("delete_actor")) return HandleDeleteActor(Request);
	if (MethodName == TEXT("delete_actors_batch")) return HandleDeleteActorsBatch(Request);
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
	if (MethodName == TEXT("place_in_grid")) return HandlePlaceInGrid(Request);
	if (MethodName == TEXT("place_along_spline")) return HandlePlaceAlongSpline(Request);
	if (MethodName == TEXT("place_in_circle")) return HandlePlaceInCircle(Request);
	if (MethodName == TEXT("scatter_in_area")) return HandleScatterInArea(Request);

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

FMCPResponse FWorldService::HandleSpawnActorsBatch(const FMCPRequest& Request)
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

	auto Task = [Specs]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		TArray<TSharedPtr<FJsonValue>> Spawned;
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (const FSpawnSpec& S : Specs)
		{
			FString Type, Err;
			AActor* A = SpawnActorInternal(World, S.ActorClass, S.Loc, S.Rot, S.Scale, Type, Err);
			if (A)
				Spawned.Add(MakeShared<FJsonValueObject>(SerializeActor(A)));
			else
				Errors.Add(MakeShared<FJsonValueString>(Err));
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
		World->DestroyActor(Actor);
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Deleted actor: %s"), *ActorName);
		return Result;
	};
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldService::HandleDeleteActorsBatch(const FMCPRequest& Request)
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

	auto Task = [Names]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		int32 Deleted = 0;
		TArray<TSharedPtr<FJsonValue>> NotFound;
		for (const FString& Name : Names)
		{
			AActor* A = FMCPActorResolver::ByLabel(World, Name);
			if (A)
			{
				World->DestroyActor(A);
				Deleted++;
			}
			else
			{
				NotFound.Add(MakeShared<FJsonValueString>(Name));
			}
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

		GEditor->SelectNone(true, true, false);
		GEditor->SelectActor(SourceActor, true, true, true);
		GEditor->edactDuplicateSelected(World->GetCurrentLevel(), false);

		AActor* NewActor = nullptr;
		USelection* Selection = GEditor->GetSelectedActors();
		if (Selection && Selection->Num() > 0)
			NewActor = Cast<AActor>(Selection->GetSelectedObject(0));

		if (!NewActor || NewActor == SourceActor)
			return FMCPJson::MakeError(TEXT("Failed to duplicate actor"));

		if (bHasNewLocation) NewActor->SetActorLocation(NewLocation);

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

		FTransform T = Actor->GetActorTransform();
		if (bLoc) T.SetLocation(Location);
		if (bRot) T.SetRotation(Rotation.Quaternion());
		if (bScale) T.SetScale3D(Scale);
		Actor->SetActorTransform(T);

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
		Actor->SetActorLocation(Location);
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
		Actor->SetActorRotation(Rotation);
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
		Actor->SetActorScale3D(Scale);
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

		void* Addr = Prop->ContainerPtrToValuePtr<void>(Actor);
		const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, Addr, Actor, PPF_None);
		if (!ImportResult)
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to import value '%s' into property '%s'"), *Value, *PropertyName));

		Actor->PostEditChange();
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
		Actor->SetActorLabel(NewLabel);
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

		int32 Applied = 0;
		for (UMeshComponent* MC : MeshComps)
		{
			MC->SetMaterial(SlotIndex, Mat);
			++Applied;
		}
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
		for (UMeshComponent* MC : MeshComps)
		{
			UMaterialInterface* CurrentMat = MC->GetMaterial(SlotIndex);
			if (!CurrentMat) continue;

			// Create a dynamic MID so editor-time writes are cheap and non-destructive.
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
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set material parameter %s on %s (%d comps)"), *ParamName, *ActorName, Updated);
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
			WS->SetFolderPath(FName(*FolderPath));
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
		Actor->SetFolderPath(FName(*FolderPath));
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
		if (!Actor->Tags.Contains(TagName)) Actor->Tags.Add(TagName);
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
		Actor->Tags.Remove(FName(*Tag));
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

FMCPResponse FWorldService::HandlePlaceInGrid(const FMCPRequest& Request)
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

	auto Task = [Base, Origin, CountX, CountY, SpacingX, SpacingY]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		TArray<FVector> Positions;
		Positions.Reserve(CountX * CountY);
		for (int32 iy = 0; iy < CountY; ++iy)
			for (int32 ix = 0; ix < CountX; ++ix)
				Positions.Add(Origin + FVector(ix * SpacingX, iy * SpacingY, 0));

		return SpawnMany(World, Base, Positions);
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

FMCPResponse FWorldService::HandleScatterInArea(const FMCPRequest& Request)
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

	auto Task = [Base, Min, Max, Count, Seed, bStickToGround]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FRandomStream Rand(Seed == 0 ? FMath::RandRange(1, 1000000) : Seed);
		TArray<FVector> Positions;
		Positions.Reserve(Count);
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
			Positions.Add(P);
		}
		return SpawnMany(World, Base, Positions);
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
		Actor->SetActorTickEnabled(bEnabled);
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
		Actor->SetActorHiddenInGame(bHidden);
		Actor->SetIsTemporarilyHiddenInEditor(bHidden);
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
		Actor->SetActorEnableCollision(bEnabled);
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

		Child->AttachToActor(Parent, Rules, FName(*SocketName));
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
		Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
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
		Tool.Description = TEXT("List actors in the current editor level. Effect: returns array of {name,class,location,rotation,scale,tags}. "
			"Params: class_filter (string, optional substring of class name), max_results (integer, default 1000). "
			"Workflow: use before spawn/delete to locate existing actors.");
		TSharedPtr<FJsonObject> ClassParam = MakeShared<FJsonObject>();
		ClassParam->SetStringField(TEXT("type"), TEXT("string"));
		ClassParam->SetStringField(TEXT("description"), TEXT("Optional class name substring filter"));
		Tool.Parameters->SetObjectField(TEXT("class_filter"), ClassParam);
		TSharedPtr<FJsonObject> MaxParam = MakeShared<FJsonObject>();
		MaxParam->SetStringField(TEXT("type"), TEXT("integer"));
		MaxParam->SetStringField(TEXT("description"), TEXT("Max actors to return (default 1000)"));
		Tool.Parameters->SetObjectField(TEXT("max_results"), MaxParam);
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("get_actor"),
		TEXT("Get detailed info about an actor by label. Effect: returns {name,class,location,rotation,scale,tags}. "
			 "Params: actor_name (string, actor label). "
			 "Workflow: pair with set_actor_* to inspect then modify."))
		.RequiredString(TEXT("actor_name"), TEXT("The actor label/name to look up"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("find_actors_by_tag"),
		TEXT("Find all actors carrying a tag. Effect: returns array of matching actors. "
			 "Params: tag (string, FName tag). "
			 "Workflow: combine with add_actor_tag to organize actors by role."))
		.RequiredString(TEXT("tag"), TEXT("The tag to search for"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_level_info");
		Tool.Description = TEXT("Return level name, path, actor count, and bounds. Effect: summary of the active editor world. "
			"Workflow: call first when starting to explore a scene.");
		Tools.Add(Tool);
	}

	// ---------- Spawn / Delete ----------
	Tools.Add(FMCPToolBuilder(TEXT("spawn_actor"),
		TEXT("Spawn an actor at a world location. Effect: creates one actor, places and returns it. "
			 "Params: actor_class (string, asset path /Game/... or class name), location ([X,Y,Z] cm), "
			 "rotation ([Pitch,Yaw,Roll] deg, optional), scale ([X,Y,Z], optional default 1,1,1). "
			 "Workflow: pair with viewport/trace_from_screen to snap to a point; screenshot to verify. "
			 "Warning: location is the pivot, not the visual center."))
		.RequiredString(TEXT("actor_class"), TEXT("Asset path (/Game/...) or class name"))
		.RequiredVec3(TEXT("location"), TEXT("World location [X, Y, Z] in cm"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale as [X, Y, Z] (default 1,1,1)"))
		.Build());

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("spawn_actors_batch");
		Tool.Description = TEXT("Spawn many actors in one call. Effect: loops per-entry spawn and returns both successes and errors. "
			"Params: spawns (array of {actor_class,location,rotation?,scale?}). "
			"Workflow: prefer over repeated spawn_actor for large placements to cut IO overhead. "
			"Warning: partial success possible; check 'errors' array.");
		TSharedPtr<FJsonObject> SpawnsParam = MakeShared<FJsonObject>();
		SpawnsParam->SetStringField(TEXT("type"), TEXT("array"));
		SpawnsParam->SetStringField(TEXT("description"), TEXT("Array of spawn specs: [{actor_class, location, rotation?, scale?}, ...]"));
		Tool.Parameters->SetObjectField(TEXT("spawns"), SpawnsParam);
		Tool.RequiredParams.Add(TEXT("spawns"));
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("delete_actor"),
		TEXT("Delete one actor by label. Effect: removes the actor from the level. "
			 "Params: actor_name (string)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label to delete"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("delete_actors_batch"),
		TEXT("Delete many actors by label. Effect: returns count deleted and not_found list. "
			 "Params: actor_names (array of strings). "
			 "Workflow: use find_actors_by_tag to gather names first."))
		.RequiredArrayOfString(TEXT("actor_names"), TEXT("Array of actor labels to delete"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("duplicate_actor"),
		TEXT("Duplicate an existing actor. Effect: copies the actor (via editor copy/paste) and returns the new label. "
			 "Params: actor_name (string), new_location ([X,Y,Z], optional)."))
		.RequiredString(TEXT("actor_name"), TEXT("The actor name to duplicate"))
		.OptionalVec3(TEXT("new_location"), TEXT("Optional new location for the duplicate"))
		.Build());

	// ---------- Transforms ----------
	Tools.Add(FMCPToolBuilder(TEXT("set_actor_transform"),
		TEXT("Set any combination of location, rotation, and scale. Effect: applies the provided components; others are preserved. "
			 "Params: actor_name (string), location ([X,Y,Z], optional), rotation ([Pitch,Yaw,Roll], optional), scale ([X,Y,Z], optional). "
			 "Workflow: prefer over three separate calls for atomic transforms."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.OptionalVec3(TEXT("location"), TEXT("World location [X,Y,Z]"))
		.OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch,Yaw,Roll] deg"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_location"),
		TEXT("Move an actor to a new world location. Effect: replaces location; rotation and scale preserved. "
			 "Params: actor_name (string), location ([X,Y,Z] cm)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredVec3(TEXT("location"), TEXT("New location [X, Y, Z] cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_rotation"),
		TEXT("Rotate an actor in world space. Effect: replaces rotation. "
			 "Params: actor_name (string), rotation ([Pitch,Yaw,Roll] deg)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredVec3(TEXT("rotation"), TEXT("Rotation [Pitch, Yaw, Roll] deg"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_scale"),
		TEXT("Scale an actor. Effect: replaces component scale. "
			 "Params: actor_name (string), scale ([X,Y,Z], 1=original)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredVec3(TEXT("scale"), TEXT("Scale [X, Y, Z]"))
		.Build());

	// ---------- Properties ----------
	Tools.Add(FMCPToolBuilder(TEXT("set_actor_property"),
		TEXT("Set any reflected UProperty on an actor via FProperty::ImportText_Direct. "
			 "Effect: parses 'value' as the property's text form. "
			 "Params: actor_name (string), property_name (string, FProperty name on the class), "
			 "value (string, UE text serialization of the value). "
			 "Warning: only works for exposed UProperties; complex structs need parenthesized tuples."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredString(TEXT("property_name"), TEXT("Reflected UProperty name"))
		.RequiredString(TEXT("value"), TEXT("String-serialized new value"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_label"),
		TEXT("Rename an actor's outliner label. Effect: changes display name in editor. "
			 "Params: actor_name (string, current label), new_label (string). "
			 "Warning: labels must be unique; UE may append a suffix if collision."))
		.RequiredString(TEXT("actor_name"), TEXT("Current actor label"))
		.RequiredString(TEXT("new_label"), TEXT("New label"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_material"),
		TEXT("Apply a material to all MeshComponents of an actor at a given slot. Effect: updates runtime material. "
			 "Params: actor_name (string), material_path (string, /Game/.../Mat.Mat), slot_index (integer, default 0)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredString(TEXT("material_path"), TEXT("Material asset path /Game/..."))
		.OptionalInteger(TEXT("slot_index"), TEXT("Material slot index (default 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_material_parameter"),
		TEXT("Set a scalar or vector parameter on an actor's current slot material (via a dynamic MID). "
			 "Effect: non-destructive runtime parameter write. "
			 "Params: actor_name (string), parameter_name (string), parameter_type ('scalar'|'vector'), "
			 "value (number for scalar, [R,G,B,A] for vector), slot_index (integer, default 0)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredString(TEXT("parameter_name"), TEXT("Material parameter name"))
		.RequiredEnum(TEXT("parameter_type"), {TEXT("scalar"), TEXT("vector")}, TEXT("Parameter type"))
		.OptionalInteger(TEXT("slot_index"), TEXT("Material slot index (default 0)"))
		.Build());

	// ---------- Organization ----------
	Tools.Add(FMCPToolBuilder(TEXT("create_folder"),
		TEXT("Create an outliner folder by setting WorldSettings's folder path. Effect: folder appears in outliner. "
			 "Params: folder_path (string, slashes for nesting)."))
		.RequiredString(TEXT("folder_path"), TEXT("Folder path, e.g. 'Lighting/Sun'"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("move_actor_to_folder"),
		TEXT("Move an actor into an outliner folder. Effect: updates actor's folder path. "
			 "Params: actor_name (string), folder_path (string)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredString(TEXT("folder_path"), TEXT("Target folder path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("add_actor_tag"),
		TEXT("Add a tag to an actor. Effect: adds FName to Actor->Tags if absent. "
			 "Params: actor_name (string), tag (string)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredString(TEXT("tag"), TEXT("Tag to add"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("remove_actor_tag"),
		TEXT("Remove a tag from an actor. Effect: removes FName from Actor->Tags if present. "
			 "Params: actor_name (string), tag (string)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredString(TEXT("tag"), TEXT("Tag to remove"))
		.Build());

	// ---------- Spatial ----------
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("measure_distance");
		Tool.Description = TEXT("Compute 3D and 2D distance between two points or actors. Effect: returns {distance,distance_2d,point_a,point_b}. "
			"Params: point_a/point_b ([X,Y,Z]) OR actor_a/actor_b (label); any combination allowed.");
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("array"));
		P->SetStringField(TEXT("description"), TEXT("Point A [X,Y,Z]"));
		Tool.Parameters->SetObjectField(TEXT("point_a"), P);
		TSharedPtr<FJsonObject> P2 = MakeShared<FJsonObject>();
		P2->SetStringField(TEXT("type"), TEXT("array"));
		P2->SetStringField(TEXT("description"), TEXT("Point B [X,Y,Z]"));
		Tool.Parameters->SetObjectField(TEXT("point_b"), P2);
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("type"), TEXT("string"));
		A->SetStringField(TEXT("description"), TEXT("Actor A label (alternative to point_a)"));
		Tool.Parameters->SetObjectField(TEXT("actor_a"), A);
		TSharedPtr<FJsonObject> A2 = MakeShared<FJsonObject>();
		A2->SetStringField(TEXT("type"), TEXT("string"));
		A2->SetStringField(TEXT("description"), TEXT("Actor B label (alternative to point_b)"));
		Tool.Parameters->SetObjectField(TEXT("actor_b"), A2);
		Tools.Add(Tool);
	}

	Tools.Add(FMCPToolBuilder(TEXT("find_actors_in_radius"),
		TEXT("Find actors within a sphere. Effect: returns matches with distance. "
			 "Params: center ([X,Y,Z]), radius (number, cm)."))
		.RequiredVec3(TEXT("center"), TEXT("Center point [X,Y,Z]"))
		.RequiredNumber(TEXT("radius"), TEXT("Radius in cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("find_actors_in_bounds"),
		TEXT("Find actors whose location is inside an axis-aligned box. Effect: returns matching actors. "
			 "Params: min/max ([X,Y,Z] cm)."))
		.RequiredVec3(TEXT("min"), TEXT("Box min corner [X,Y,Z]"))
		.RequiredVec3(TEXT("max"), TEXT("Box max corner [X,Y,Z]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("raycast"),
		TEXT("Line-trace from start to end against visibility channel. Effect: returns {hit,location?,normal?,distance?,actor_name?}. "
			 "Params: start/end ([X,Y,Z] cm). "
			 "Workflow: use to test line-of-sight or find surface points."))
		.RequiredVec3(TEXT("start"), TEXT("Start point [X,Y,Z]"))
		.RequiredVec3(TEXT("end"), TEXT("End point [X,Y,Z]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("get_ground_height"),
		TEXT("Downward raycast from high Z at (x,y) to find ground Z. Effect: returns {hit,z,location?,normal?}. "
			 "Params: x,y (number, cm world), max_z (optional, default 100000). "
			 "Workflow: pair with spawn_actor to place props on terrain."))
		.RequiredNumber(TEXT("x"), TEXT("X world coordinate cm"))
		.RequiredNumber(TEXT("y"), TEXT("Y world coordinate cm"))
		.OptionalNumber(TEXT("max_z"), TEXT("Max Z range for trace (default 100000)"))
		.Build());

	// ---------- Pattern placement ----------
	Tools.Add(FMCPToolBuilder(TEXT("place_in_grid"),
		TEXT("Spawn a grid of actors. Effect: CountX*CountY actors spaced by SpacingX/Y around origin. "
			 "Params: actor_class, origin ([X,Y,Z]), count_x/count_y (integer), spacing_x/y (number), rotation?, scale?."))
		.RequiredString(TEXT("actor_class"), TEXT("Asset path or class name"))
		.RequiredVec3(TEXT("origin"), TEXT("Grid origin [X,Y,Z]"))
		.RequiredInteger(TEXT("count_x"), TEXT("Grid columns"))
		.RequiredInteger(TEXT("count_y"), TEXT("Grid rows"))
		.OptionalNumber(TEXT("spacing_x"), TEXT("Column spacing cm (default 100)"))
		.OptionalNumber(TEXT("spacing_y"), TEXT("Row spacing cm (default 100)"))
		.OptionalVec3(TEXT("rotation"), TEXT("Base rotation [Pitch,Yaw,Roll]"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("place_along_spline"),
		TEXT("Spawn N actors along an existing spline actor, evenly by length. Effect: positions are sampled uniformly. "
			 "Params: actor_class, spline_actor (label of actor with USplineComponent), count (integer), "
			 "align_to_spline (bool, default false), rotation?, scale?. "
			 "Warning: referenced actor must have a USplineComponent."))
		.RequiredString(TEXT("actor_class"), TEXT("Asset path or class name"))
		.RequiredString(TEXT("spline_actor"), TEXT("Label of actor with a USplineComponent"))
		.RequiredInteger(TEXT("count"), TEXT("Number of actors to spawn"))
		.OptionalBool(TEXT("align_to_spline"), TEXT("Orient each actor to spline tangent"))
		.OptionalVec3(TEXT("rotation"), TEXT("Base rotation (overridden when align_to_spline=true)"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("place_in_circle"),
		TEXT("Spawn N actors evenly distributed on a circle. Effect: returns spawned actor list. "
			 "Params: actor_class, center ([X,Y,Z]), radius (number), count (integer), face_outward (bool, optional)."))
		.RequiredString(TEXT("actor_class"), TEXT("Asset path or class name"))
		.RequiredVec3(TEXT("center"), TEXT("Circle center [X,Y,Z]"))
		.RequiredNumber(TEXT("radius"), TEXT("Circle radius in cm"))
		.RequiredInteger(TEXT("count"), TEXT("Number of actors"))
		.OptionalBool(TEXT("face_outward"), TEXT("Rotate each actor to face away from center"))
		.OptionalVec3(TEXT("rotation"), TEXT("Base rotation (overridden when face_outward=true)"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("scatter_in_area"),
		TEXT("Randomly scatter N actors in an axis-aligned box. Effect: uses seeded RNG; optionally snaps to ground. "
			 "Params: actor_class, min/max ([X,Y,Z]), count (integer), seed (integer, optional), "
			 "stick_to_ground (bool, optional), rotation?, scale?."))
		.RequiredString(TEXT("actor_class"), TEXT("Asset path or class name"))
		.RequiredVec3(TEXT("min"), TEXT("Box min corner [X,Y,Z]"))
		.RequiredVec3(TEXT("max"), TEXT("Box max corner [X,Y,Z]"))
		.RequiredInteger(TEXT("count"), TEXT("Number of actors to scatter"))
		.OptionalInteger(TEXT("seed"), TEXT("RNG seed (0 = random)"))
		.OptionalBool(TEXT("stick_to_ground"), TEXT("Raycast down to snap each actor to ground"))
		.OptionalVec3(TEXT("rotation"), TEXT("Base rotation"))
		.OptionalVec3(TEXT("scale"), TEXT("Scale [X,Y,Z]"))
		.Build());

	// ---------- Actor state ----------
	Tools.Add(FMCPToolBuilder(TEXT("set_actor_tick_enabled"),
		TEXT("Enable/disable an actor's tick function. Effect: toggles Actor::SetActorTickEnabled. "
			 "Params: actor_name (string), enabled (bool)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredBool(TEXT("enabled"), TEXT("true=tick on, false=tick off"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_hidden"),
		TEXT("Hide/show an actor in both editor and game. Effect: toggles SetActorHiddenInGame and SetIsTemporarilyHiddenInEditor. "
			 "Params: actor_name (string), hidden (bool)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredBool(TEXT("hidden"), TEXT("true=hide, false=show"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_actor_collision"),
		TEXT("Enable/disable an actor's collision. Effect: Actor::SetActorEnableCollision. "
			 "Params: actor_name (string), enabled (bool)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.RequiredBool(TEXT("enabled"), TEXT("true=enable, false=disable"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("attach_to"),
		TEXT("Attach one actor to another. Effect: child becomes parented to parent. "
			 "Params: child (string, label), parent (string, label), socket (string, optional), "
			 "rule (enum 'keep_world'|'keep_relative'|'snap_to_target', default 'keep_world')."))
		.RequiredString(TEXT("child"), TEXT("Child actor label"))
		.RequiredString(TEXT("parent"), TEXT("Parent actor label"))
		.OptionalString(TEXT("socket"), TEXT("Optional socket name on parent's root"))
		.OptionalEnum(TEXT("rule"), {TEXT("keep_world"), TEXT("keep_relative"), TEXT("snap_to_target")},
			TEXT("Attachment transform rule (default keep_world)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("detach"),
		TEXT("Detach an actor from its parent. Effect: keeps world transform. "
			 "Params: actor_name (string)."))
		.RequiredString(TEXT("actor_name"), TEXT("Actor label"))
		.Build());

	return Tools;
}

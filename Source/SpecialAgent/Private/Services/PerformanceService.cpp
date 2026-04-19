// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PerformanceService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "WorldCollision.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "HAL/PlatformMemory.h"

FPerformanceService::FPerformanceService()
{
}

FString FPerformanceService::GetServiceDescription() const
{
	return TEXT("Performance analysis - level statistics, bounds checking, overlap detection, triangle counts, and draw-call estimates.");
}

FMCPResponse FPerformanceService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("get_statistics"))         return HandleGetStatistics(Request);
	if (MethodName == TEXT("get_actor_bounds"))       return HandleGetActorBounds(Request);
	if (MethodName == TEXT("check_overlaps"))         return HandleCheckOverlaps(Request);
	if (MethodName == TEXT("get_triangle_count"))     return HandleGetTriangleCount(Request);
	if (MethodName == TEXT("get_draw_call_estimate")) return HandleGetDrawCallEstimate(Request);

	return MethodNotFound(Request.Id, TEXT("performance"), MethodName);
}

namespace
{
	// Returns LOD0 triangle count for a static mesh, or 0 if unavailable.
	int32 StaticMeshLod0Triangles(const UStaticMesh* Mesh)
	{
		if (!Mesh) return 0;
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0) return 0;
		return RenderData->LODResources[0].GetNumTriangles();
	}
}

// -----------------------------------------------------------------------------
// get_statistics
// -----------------------------------------------------------------------------
FMCPResponse FPerformanceService::HandleGetStatistics(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world"));
			return Result;
		}

		int32 ActorCount = 0;
		int64 TotalTriangles = 0;
		TSet<TPair<const UStaticMesh*, const UMaterialInterface*>> DistinctPairs;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			++ActorCount;

			TArray<UStaticMeshComponent*> Comps;
			Actor->GetComponents<UStaticMeshComponent>(Comps);
			for (UStaticMeshComponent* Comp : Comps)
			{
				if (!Comp) continue;
				const UStaticMesh* Mesh = Comp->GetStaticMesh();
				if (!Mesh) continue;
				TotalTriangles += StaticMeshLod0Triangles(Mesh);

				const int32 NumMats = Comp->GetNumMaterials();
				if (NumMats == 0)
				{
					DistinctPairs.Add(TPair<const UStaticMesh*, const UMaterialInterface*>(Mesh, nullptr));
				}
				else
				{
					for (int32 i = 0; i < NumMats; ++i)
					{
						DistinctPairs.Add(TPair<const UStaticMesh*, const UMaterialInterface*>(Mesh, Comp->GetMaterial(i)));
					}
				}
			}
		}

		const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("actor_count"), ActorCount);
		Result->SetNumberField(TEXT("triangle_count_lod0"), static_cast<double>(TotalTriangles));
		Result->SetNumberField(TEXT("draw_call_estimate"), DistinctPairs.Num());
		Result->SetNumberField(TEXT("process_memory_used_bytes"), static_cast<double>(MemStats.UsedPhysical));
		Result->SetNumberField(TEXT("process_memory_peak_bytes"), static_cast<double>(MemStats.PeakUsedPhysical));

		UE_LOG(LogTemp, Log,
			TEXT("SpecialAgent: performance/get_statistics — %d actors, %lld tris, %d draw-call pairs"),
			ActorCount, (long long)TotalTriangles, DistinctPairs.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// get_actor_bounds
// -----------------------------------------------------------------------------
FMCPResponse FPerformanceService::HandleGetActorBounds(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString ActorName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	}

	bool bIncludeNonColliding = true;
	FMCPJson::ReadBool(Request.Params, TEXT("include_non_colliding"), bIncludeNonColliding);

	auto Task = [ActorName, bIncludeNonColliding]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world"));
			return Result;
		}

		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
			return Result;
		}

		const FBox Box = Actor->GetComponentsBoundingBox(bIncludeNonColliding);
		if (!Box.IsValid)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Actor has no valid component bounds"));
			return Result;
		}

		TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
		FMCPJson::WriteVec3(BoundsObj, TEXT("min"),    Box.Min);
		FMCPJson::WriteVec3(BoundsObj, TEXT("max"),    Box.Max);
		FMCPJson::WriteVec3(BoundsObj, TEXT("center"), Box.GetCenter());
		FMCPJson::WriteVec3(BoundsObj, TEXT("size"),   Box.GetSize());
		FMCPJson::WriteVec3(BoundsObj, TEXT("extent"), Box.GetExtent());

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
		Result->SetObjectField(TEXT("bounds"), BoundsObj);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: performance/get_actor_bounds %s"), *ActorName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// check_overlaps
// -----------------------------------------------------------------------------
FMCPResponse FPerformanceService::HandleCheckOverlaps(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FVector Center;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("center"), Center))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'center' [X, Y, Z]"));
	}

	FVector HalfExtent(50.0f, 50.0f, 50.0f);
	FMCPJson::ReadVec3(Request.Params, TEXT("half_extent"), HalfExtent);

	FRotator Rotation = FRotator::ZeroRotator;
	FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

	auto Task = [Center, HalfExtent, Rotation]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world"));
			return Result;
		}

		TArray<FOverlapResult> Overlaps;
		const FCollisionShape Shape = FCollisionShape::MakeBox(HalfExtent);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PerformanceCheckOverlaps), /*bTraceComplex=*/false);
		const bool bAnyBlocking = World->OverlapMultiByChannel(
			Overlaps, Center, Rotation.Quaternion(), ECC_WorldStatic, Shape, Params);

		TArray<TSharedPtr<FJsonValue>> ActorsJson;
		TSet<AActor*> Seen;
		for (const FOverlapResult& Res : Overlaps)
		{
			AActor* Actor = Res.GetActor();
			if (!Actor || Seen.Contains(Actor)) continue;
			Seen.Add(Actor);

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Actor->GetActorLabel());
			Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			FMCPJson::WriteVec3(Obj, TEXT("location"), Actor->GetActorLocation());
			ActorsJson.Add(MakeShared<FJsonValueObject>(Obj));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("any_blocking"), bAnyBlocking);
		Result->SetNumberField(TEXT("count"), ActorsJson.Num());
		Result->SetArrayField(TEXT("actors"), ActorsJson);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: performance/check_overlaps returned %d actors"), ActorsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// get_triangle_count
// -----------------------------------------------------------------------------
FMCPResponse FPerformanceService::HandleGetTriangleCount(const FMCPRequest& Request)
{
	FBox Bounds(ForceInit);
	bool bHasBounds = false;

	if (Request.Params.IsValid())
	{
		FVector BoundsMin, BoundsMax;
		const bool bHasMin = FMCPJson::ReadVec3(Request.Params, TEXT("bounds_min"), BoundsMin);
		const bool bHasMax = FMCPJson::ReadVec3(Request.Params, TEXT("bounds_max"), BoundsMax);
		if (bHasMin && bHasMax)
		{
			Bounds = FBox(BoundsMin, BoundsMax);
			bHasBounds = true;
		}
	}

	auto Task = [Bounds, bHasBounds]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world"));
			return Result;
		}

		int64 TotalTriangles = 0;
		int32 ConsideredActors = 0;
		int32 ConsideredMeshes = 0;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			if (bHasBounds)
			{
				const FBox ActorBox = Actor->GetComponentsBoundingBox(true);
				if (!ActorBox.IsValid || !Bounds.Intersect(ActorBox)) continue;
			}
			++ConsideredActors;

			TArray<UStaticMeshComponent*> Comps;
			Actor->GetComponents<UStaticMeshComponent>(Comps);
			for (UStaticMeshComponent* Comp : Comps)
			{
				if (!Comp) continue;
				const UStaticMesh* Mesh = Comp->GetStaticMesh();
				if (!Mesh) continue;
				++ConsideredMeshes;
				TotalTriangles += StaticMeshLod0Triangles(Mesh);
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("triangle_count_lod0"), static_cast<double>(TotalTriangles));
		Result->SetNumberField(TEXT("actors_considered"), ConsideredActors);
		Result->SetNumberField(TEXT("static_mesh_components"), ConsideredMeshes);
		Result->SetBoolField(TEXT("bounded"), bHasBounds);

		UE_LOG(LogTemp, Log,
			TEXT("SpecialAgent: performance/get_triangle_count — %lld tris across %d actors (bounded=%s)"),
			(long long)TotalTriangles, ConsideredActors, bHasBounds ? TEXT("true") : TEXT("false"));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// get_draw_call_estimate
// -----------------------------------------------------------------------------
FMCPResponse FPerformanceService::HandleGetDrawCallEstimate(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world"));
			return Result;
		}

		TSet<TPair<const UStaticMesh*, const UMaterialInterface*>> DistinctPairs;
		int32 SMCComponents = 0;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;

			TArray<UStaticMeshComponent*> Comps;
			Actor->GetComponents<UStaticMeshComponent>(Comps);
			for (UStaticMeshComponent* Comp : Comps)
			{
				if (!Comp) continue;
				const UStaticMesh* Mesh = Comp->GetStaticMesh();
				if (!Mesh) continue;
				++SMCComponents;

				const int32 NumMats = Comp->GetNumMaterials();
				if (NumMats == 0)
				{
					DistinctPairs.Add(TPair<const UStaticMesh*, const UMaterialInterface*>(Mesh, nullptr));
				}
				else
				{
					for (int32 i = 0; i < NumMats; ++i)
					{
						DistinctPairs.Add(TPair<const UStaticMesh*, const UMaterialInterface*>(Mesh, Comp->GetMaterial(i)));
					}
				}
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("draw_call_estimate"), DistinctPairs.Num());
		Result->SetNumberField(TEXT("static_mesh_components"), SMCComponents);
		Result->SetStringField(TEXT("method"), TEXT("distinct (StaticMesh, Material) pairs across SMCs"));

		UE_LOG(LogTemp, Log,
			TEXT("SpecialAgent: performance/get_draw_call_estimate — %d distinct pairs across %d SMCs"),
			DistinctPairs.Num(), SMCComponents);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// Tool schemas
// -----------------------------------------------------------------------------
TArray<FMCPToolInfo> FPerformanceService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
			TEXT("get_statistics"),
			TEXT("Summarize level performance. Counts actors, sums LOD0 triangle count across all static-mesh components, estimates draw calls as distinct (StaticMesh, Material) pairs, and reports process memory. "
			     "Params: none. "
			     "Workflow: run before and after large edits to measure impact. "
			     "Warning: draw-call estimate ignores instancing and is an upper bound."))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_actor_bounds"),
			TEXT("Get axis-aligned bounds of one actor. Returns min/max/center/size/extent computed via GetComponentsBoundingBox. "
			     "Params: actor_name (string, label), include_non_colliding (bool, optional, default true — include components that don't collide). "
			     "Workflow: use to size volumes or find placement clearance. "
			     "Warning: not oriented — rotating the actor invalidates previous values."))
		.RequiredString(TEXT("actor_name"),            TEXT("Actor label as shown in the World Outliner"))
		.OptionalBool  (TEXT("include_non_colliding"), TEXT("Include components without collision in the box; default true"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("check_overlaps"),
			TEXT("Find actors overlapping a box. Runs World->OverlapMultiByChannel on the WorldStatic channel at the specified transform. Returns actor name, class, and location for each unique actor hit. "
			     "Params: center ([X,Y,Z] cm world), half_extent ([X,Y,Z] cm, optional, default 50,50,50), rotation ([Pitch,Yaw,Roll] deg, optional, default zero). "
			     "Workflow: use before spawning to detect collisions; pair with assets/get_bounds to size the query box. "
			     "Warning: only reports against the WorldStatic channel — dynamic actors may be missed if their collision profile excludes it."))
		.RequiredVec3(TEXT("center"),      TEXT("Box center as [X, Y, Z] in cm"))
		.OptionalVec3(TEXT("half_extent"), TEXT("Box half-extent as [X, Y, Z] in cm; default 50,50,50"))
		.OptionalVec3(TEXT("rotation"),    TEXT("Optional box rotation as [Pitch, Yaw, Roll] in degrees"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_triangle_count"),
			TEXT("Count LOD0 triangles across static meshes. Iterates all actors (optionally filtered to those whose component bounds intersect a box) and sums UStaticMesh::GetNumTriangles(0) across each actor's static-mesh components. "
			     "Params: bounds_min ([X,Y,Z] cm, optional), bounds_max ([X,Y,Z] cm, optional — both must be provided to enable the filter). "
			     "Workflow: pair with viewport/get_transform to target a camera frustum, or use get_actor_bounds to derive box limits. "
			     "Warning: LOD0 is the heaviest LOD; a runtime cost will be lower."))
		.OptionalVec3(TEXT("bounds_min"), TEXT("Optional AABB min [X,Y,Z] cm; must be paired with bounds_max"))
		.OptionalVec3(TEXT("bounds_max"), TEXT("Optional AABB max [X,Y,Z] cm; must be paired with bounds_min"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_draw_call_estimate"),
			TEXT("Estimate draw calls from distinct (StaticMesh, Material) pairs across all static-mesh components. "
			     "Params: none. "
			     "Workflow: run post-layout to spot high draw-call scenes — mergeable meshes and shared materials bring the number down. "
			     "Warning: ignores ISM/HISM instancing, Nanite batching, and particle systems; treat as an upper-bound heuristic."))
		.Build());

	return Tools;
}

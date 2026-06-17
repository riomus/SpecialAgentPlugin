// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PerformanceService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"
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

FMCPResponse FPerformanceService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
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
			TEXT("Summarize editor-world performance in one call: counts actors, sums LOD0 triangles across every static-mesh component, estimates draw calls as distinct (StaticMesh, Material) pairs, and reports process memory. "
			     "Returns {actor_count, triangle_count_lod0, draw_call_estimate, process_memory_used_bytes, process_memory_peak_bytes}. "
			     "Params: (none). "
			     "Workflow: Read-only; run before and after large edits to measure impact. For just triangles use get_triangle_count, for just draw calls use get_draw_call_estimate. "
			     "Warning: Iterates all editor-world actors (cost scales with actor count); the draw-call estimate ignores ISM/HISM instancing and Nanite, so it is an upper bound; memory figures are whole-process, not per-level."))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_actor_bounds"),
			TEXT("Get the world-space axis-aligned bounding box of one actor, resolved by its World Outliner label via GetComponentsBoundingBox. "
			     "Returns {actor_name, bounds:{min, max, center, size, extent}} all in world-space cm. "
			     "Params: actor_name (string, required; the actor's display label from the World Outliner), include_non_colliding (bool, optional, default true; include components that have no collision). "
			     "Workflow: Read-only; use to size streaming volumes, feed a box into check_overlaps / get_triangle_count, or find placement clearance. "
			     "Warning: The box is axis-aligned (not oriented), so a rotated actor yields a looser box; returns success=false if the label is not found or the actor has no valid bounds."))
		.RequiredString(TEXT("actor_name"),            TEXT("Actor label as shown in the World Outliner"))
		.OptionalBool  (TEXT("include_non_colliding"), TEXT("Include components without collision in the box; default true"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("check_overlaps"),
			TEXT("Find actors whose collision overlaps an oriented box, via World OverlapMultiByChannel on the WorldStatic channel. "
			     "Returns {any_blocking, count, actors:[{name (label), class, location (world cm)}]} (each overlapped actor reported once). "
			     "Params: center ([X,Y,Z] world-space cm, required, box center), half_extent ([X,Y,Z] cm half-size, optional, default [50,50,50]), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0]). "
			     "Workflow: Use before spawning to detect collisions; size the box from get_actor_bounds (extent field). "
			     "Warning: Read-only query; only tests the WorldStatic channel, so actors whose collision profile excludes WorldStatic (e.g. some movable/dynamic actors) are missed."))
		.RequiredVec3(TEXT("center"),      TEXT("Box center [X,Y,Z] world cm"))
		.OptionalVec3(TEXT("half_extent"), TEXT("Box half-extent [X,Y,Z] cm; default [50,50,50]"))
		.OptionalVec3(TEXT("rotation"),    TEXT("Box rotation [Pitch,Yaw,Roll] degrees; default [0,0,0]"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_triangle_count"),
			TEXT("Sum LOD0 render triangles across static-mesh components in the editor world, optionally limited to actors whose bounds intersect a box. "
			     "Returns {triangle_count_lod0, actors_considered, static_mesh_components, bounded}. "
			     "Params: bounds_min ([X,Y,Z] world-space cm, optional), bounds_max ([X,Y,Z] world-space cm, optional). Supply BOTH to enable the AABB filter; if either is missing the whole level is counted. "
			     "Workflow: Read-only; derive a box from get_actor_bounds (min/max) to measure a sub-region. "
			     "Warning: Counts LOD0 (heaviest LOD) only, so this overstates rendered cost at distance and ignores ISM/HISM instance multiplicity; the box test uses each actor's full bounds, so meshes straddling the edge are included."))
		.OptionalVec3(TEXT("bounds_min"), TEXT("AABB min [X,Y,Z] world cm; must be paired with bounds_max"))
		.OptionalVec3(TEXT("bounds_max"), TEXT("AABB max [X,Y,Z] world cm; must be paired with bounds_min"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_draw_call_estimate"),
			TEXT("Estimate draw calls as the number of distinct (StaticMesh, Material) pairs across every static-mesh component in the editor world. "
			     "Returns {draw_call_estimate, static_mesh_components, method}. "
			     "Params: (none). "
			     "Workflow: Read-only; run post-layout to spot high draw-call scenes (merging meshes and sharing materials lowers the number). For the same value plus triangles/memory use get_statistics instead. "
			     "Warning: Ignores ISM/HISM instancing, Nanite batching, skeletal meshes, and particle systems, so it is an upper-bound heuristic, not the real RHI draw-call count."))
		.Build());

	return Tools;
}

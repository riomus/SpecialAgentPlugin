// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/NavigationService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "Editor.h"
#include "EditorBuildUtils.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavMesh/NavMeshBoundsVolume.h"

FNavigationService::FNavigationService()
{
}

FString FNavigationService::GetServiceDescription() const
{
	return TEXT("Navigation mesh management - rebuild navmesh, test synchronous paths, query navmesh bounds and projected points");
}

FMCPResponse FNavigationService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("rebuild_navmesh")) return HandleRebuildNavMesh(Request);
	if (MethodName == TEXT("test_path")) return HandleTestPath(Request);
	if (MethodName == TEXT("get_navmesh_bounds")) return HandleGetNavMeshBounds(Request);
	if (MethodName == TEXT("find_nearest_reachable_point")) return HandleFindNearestReachablePoint(Request);

	return MethodNotFound(Request.Id, TEXT("navigation"), MethodName);
}

FMCPResponse FNavigationService::HandleRebuildNavMesh(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
		if (!NavSys)
		{
			return FMCPJson::MakeError(TEXT("No UNavigationSystemV1 on the world"));
		}

		// Prefer the editor build path so dirty state is cleared and feedback
		// is consistent with the editor Build menu.
		const bool bEditorBuildSucceeded = FEditorBuildUtils::EditorBuild(
			World, FBuildOptions::BuildAIPaths, /*bAllowLightingDialog=*/false);

		if (!bEditorBuildSucceeded)
		{
			// Fall back to the direct API if the editor build pipeline refused.
			NavSys->Build();
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("editor_build_invoked"), bEditorBuildSucceeded);
		Result->SetNumberField(TEXT("remaining_build_tasks"), NavSys->GetNumRemainingBuildTasks());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: navigation/rebuild_navmesh (editor_build=%s, remaining=%d)"),
			bEditorBuildSucceeded ? TEXT("ok") : TEXT("fallback"), NavSys->GetNumRemainingBuildTasks());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNavigationService::HandleTestPath(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FVector Start, End;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("start"), Start))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'start' ([X,Y,Z] world cm)"));
	}
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("end"), End))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'end' ([X,Y,Z] world cm)"));
	}

	auto Task = [Start, End]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Start, End);
		if (!Path)
		{
			return FMCPJson::MakeError(TEXT("FindPathToLocationSynchronously returned null (no nav data?)"));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("is_valid"), Path->IsValid());
		Result->SetBoolField(TEXT("is_partial"), Path->IsPartial());
		Result->SetNumberField(TEXT("path_length"), Path->GetPathLength());

		TArray<TSharedPtr<FJsonValue>> Waypoints;
		for (const FVector& Pt : Path->PathPoints)
		{
			TSharedPtr<FJsonObject> PtObj = MakeShared<FJsonObject>();
			FMCPJson::WriteVec3(PtObj, TEXT("point"), Pt);
			Waypoints.Add(MakeShared<FJsonValueObject>(PtObj));
		}
		Result->SetArrayField(TEXT("waypoints"), Waypoints);
		Result->SetNumberField(TEXT("waypoint_count"), Waypoints.Num());
		FMCPJson::WriteVec3(Result, TEXT("start"), Start);
		FMCPJson::WriteVec3(Result, TEXT("end"), End);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: navigation/test_path valid=%d partial=%d points=%d length=%.1f"),
			Path->IsValid() ? 1 : 0, Path->IsPartial() ? 1 : 0, Waypoints.Num(), Path->GetPathLength());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNavigationService::HandleGetNavMeshBounds(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		FBox Combined(ForceInit);
		TArray<TSharedPtr<FJsonValue>> VolumesJson;

		for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
		{
			ANavMeshBoundsVolume* Volume = *It;
			if (!Volume) continue;

			FVector Origin, Extent;
			Volume->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
			const FBox VolumeBox(Origin - Extent, Origin + Extent);
			Combined += VolumeBox;

			TSharedPtr<FJsonObject> VolumeObj = MakeShared<FJsonObject>();
			VolumeObj->SetStringField(TEXT("label"), Volume->GetActorLabel());
			FMCPJson::WriteVec3(VolumeObj, TEXT("origin"), Origin);
			FMCPJson::WriteVec3(VolumeObj, TEXT("extent"), Extent);
			FMCPJson::WriteVec3(VolumeObj, TEXT("min"), VolumeBox.Min);
			FMCPJson::WriteVec3(VolumeObj, TEXT("max"), VolumeBox.Max);
			VolumesJson.Add(MakeShared<FJsonValueObject>(VolumeObj));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("volumes"), VolumesJson);
		Result->SetNumberField(TEXT("volume_count"), VolumesJson.Num());

		if (Combined.IsValid)
		{
			FMCPJson::WriteVec3(Result, TEXT("combined_min"), Combined.Min);
			FMCPJson::WriteVec3(Result, TEXT("combined_max"), Combined.Max);
			FMCPJson::WriteVec3(Result, TEXT("combined_center"), Combined.GetCenter());
			FMCPJson::WriteVec3(Result, TEXT("combined_extent"), Combined.GetExtent());
		}

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: navigation/get_navmesh_bounds found %d volume(s)"), VolumesJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNavigationService::HandleFindNearestReachablePoint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FVector Point;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("point"), Point))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'point' ([X,Y,Z] world cm)"));
	}

	FVector QueryExtent(500.0, 500.0, 500.0);
	FMCPJson::ReadVec3(Request.Params, TEXT("query_extent"), QueryExtent);

	auto Task = [Point, QueryExtent]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
		if (!NavSys)
		{
			return FMCPJson::MakeError(TEXT("No UNavigationSystemV1 on the world"));
		}

		FNavLocation Projected;
		const bool bFound = NavSys->ProjectPointToNavigation(Point, Projected, QueryExtent);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetBoolField(TEXT("found"), bFound);
		FMCPJson::WriteVec3(Result, TEXT("input_point"), Point);
		FMCPJson::WriteVec3(Result, TEXT("query_extent"), QueryExtent);

		if (bFound)
		{
			FMCPJson::WriteVec3(Result, TEXT("projected_point"), Projected.Location);
			Result->SetNumberField(TEXT("distance"), FVector::Distance(Point, Projected.Location));
		}

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: navigation/find_nearest_reachable_point found=%d input=(%.1f,%.1f,%.1f)"),
			bFound ? 1 : 0, Point.X, Point.Y, Point.Z);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FNavigationService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
		TEXT("rebuild_navmesh"),
		TEXT("Rebuild the navigation mesh for the current editor world. Invokes FEditorBuildUtils::EditorBuild (NAME_BuildAIPaths) and falls back to UNavigationSystemV1::Build.\n"
		     "Params: (none).\n"
		     "Workflow: Call after adding or modifying ANavMeshBoundsVolume, geometry, or agent settings.\n"
		     "Warning: Build is synchronous from the request's perspective but remaining_build_tasks may be non-zero on return for async tiles."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("test_path"),
		TEXT("Find a synchronous navmesh path from start to end. Returns waypoints, length, and validity flags.\n"
		     "Params: start ([X,Y,Z] cm, required), end ([X,Y,Z] cm, required).\n"
		     "Workflow: Use after rebuild_navmesh or when debugging AI reachability.\n"
		     "Warning: Returns is_valid=false when no nav data covers the points; call find_nearest_reachable_point first to snap onto the navmesh."))
		.RequiredVec3(TEXT("start"), TEXT("Path start [X,Y,Z] world cm"))
		.RequiredVec3(TEXT("end"), TEXT("Path end [X,Y,Z] world cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("get_navmesh_bounds"),
		TEXT("List all ANavMeshBoundsVolume actors with their bounds, plus a combined min/max encompassing them.\n"
		     "Params: (none).\n"
		     "Workflow: Inspect navmesh coverage before running test_path or placing AI."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("find_nearest_reachable_point"),
		TEXT("Project a world-space point onto the nearest navmesh location using ProjectPointToNavigation.\n"
		     "Params: point ([X,Y,Z] cm, required), query_extent ([X,Y,Z] cm, optional, default [500,500,500]).\n"
		     "Workflow: Use to snap spawn locations or pathing endpoints onto reachable nav surfaces."))
		.RequiredVec3(TEXT("point"), TEXT("World-space point to project [X,Y,Z] cm"))
		.OptionalVec3(TEXT("query_extent"), TEXT("Search box half-extents [X,Y,Z] cm (default 500,500,500)"))
		.Build());

	return Tools;
}

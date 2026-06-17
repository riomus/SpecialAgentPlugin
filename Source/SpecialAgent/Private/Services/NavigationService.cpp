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
	if (MethodName == TEXT("rebuild_navmesh")) return HandleRebuildNavMesh(Request, Ctx);
	if (MethodName == TEXT("test_path")) return HandleTestPath(Request);
	if (MethodName == TEXT("get_navmesh_bounds")) return HandleGetNavMeshBounds(Request);
	if (MethodName == TEXT("find_nearest_reachable_point")) return HandleFindNearestReachablePoint(Request);

	return MethodNotFound(Request.Id, TEXT("navigation"), MethodName);
}

FMCPResponse FNavigationService::HandleRebuildNavMesh(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	const auto SendProgress = Ctx.SendProgress;
	SendProgress(0.0, 1.0, TEXT("navmesh build starting"));

	// Step 1: kick the build on the game thread, capture initial task counts.
	struct FKickResult
	{
		bool bEditorBuildInvoked = false;
		int32 InitialRemaining = 0;
		bool bFailed = false;
		FString Error;
	};

	auto KickTask = []() -> FKickResult
	{
		FKickResult R;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			R.bFailed = true;
			R.Error = TEXT("No editor world found");
			return R;
		}

		UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
		if (!NavSys)
		{
			R.bFailed = true;
			R.Error = TEXT("No UNavigationSystemV1 on the world");
			return R;
		}

		// Prefer the editor build path so dirty state is cleared and feedback
		// is consistent with the editor Build menu.
		R.bEditorBuildInvoked = FEditorBuildUtils::EditorBuild(
			World, FBuildOptions::BuildAIPaths, /*bAllowLightingDialog=*/false);

		if (!R.bEditorBuildInvoked)
		{
			// Fall back to the direct API if the editor build pipeline refused.
			NavSys->Build();
		}

		// Use at least 1 so progress math is safe when tasks are already queued.
		R.InitialRemaining = FMath::Max(1,
			NavSys->GetNumRemainingBuildTasks() + NavSys->GetNumRunningBuildTasks());
		return R;
	};

	// The navmesh rebuild kick runs FEditorBuildUtils::EditorBuild / NavSys->Build
	// synchronously on the game thread and can block well past the default 120s
	// bound on large levels; allow up to 30 minutes so a real build is not turned
	// into a false timeout error. (The per-poll tasks below stay on the default.)
	const FKickResult Kick = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<FKickResult>(KickTask, 1800.0);

	if (Kick.bFailed)
	{
		TSharedPtr<FJsonObject> ErrResult = FMCPJson::MakeError(Kick.Error);
		return FMCPResponse::Success(Request.Id, ErrResult);
	}

	// Step 2: poll on the background thread until all tasks complete or timeout.
	struct FPollState
	{
		int32 Remaining = 0;
		int32 Running   = 0;
		bool bValid     = false;
	};

	constexpr double TimeoutSeconds = 300.0;
	const double StartTime = FPlatformTime::Seconds();
	bool bTimedOut = false;
	FPollState LastPoll;

	while (true)
	{
		auto PollTask = []() -> FPollState
		{
			FPollState S;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) return S;
			UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
			if (!NavSys) return S;
			S.Remaining = NavSys->GetNumRemainingBuildTasks();
			S.Running   = NavSys->GetNumRunningBuildTasks();
			S.bValid    = true;
			return S;
		};

		const FPollState S = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<FPollState>(PollTask);
		LastPoll = S;
		if (!S.bValid) break;

		const int32 Outstanding = S.Remaining + S.Running;
		if (Outstanding <= 0) break;

		const double Progress = FMath::Clamp(
			double(Kick.InitialRemaining - Outstanding) / double(Kick.InitialRemaining),
			0.0, 1.0);
		SendProgress(Progress, 1.0,
			FString::Printf(TEXT("navmesh build: %d running, %d remaining"), S.Running, S.Remaining));

		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			bTimedOut = true;
			break;
		}

		FPlatformProcess::Sleep(0.5f);
	}

	SendProgress(1.0, 1.0, bTimedOut
		? TEXT("navmesh build: timeout waiting for completion")
		: TEXT("navmesh build complete"));

	// Build JSON response preserving existing response shape.
	TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
	Result->SetBoolField(TEXT("editor_build_invoked"), Kick.bEditorBuildInvoked);
	Result->SetNumberField(TEXT("remaining_build_tasks"), LastPoll.bValid ? LastPoll.Remaining : 0);
	if (bTimedOut)
	{
		Result->SetBoolField(TEXT("timed_out"), true);
	}

	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: navigation/rebuild_navmesh (editor_build=%s, timed_out=%s, remaining=%d)"),
		Kick.bEditorBuildInvoked ? TEXT("ok") : TEXT("fallback"),
		bTimedOut ? TEXT("true") : TEXT("false"),
		LastPoll.bValid ? LastPoll.Remaining : 0);

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
		TEXT("Rebuild the navigation mesh for the current editor world, then poll until build tasks drain (300s cap). "
		     "Invokes FEditorBuildUtils::EditorBuild (FBuildOptions::BuildAIPaths) and falls back to UNavigationSystemV1::Build. "
		     "Returns {success, editor_build_invoked:bool, remaining_build_tasks:int, timed_out:bool (only when it times out)}. "
		     "Params: (none). "
		     "Workflow: a NavMeshBoundsVolume + generated RecastNavMesh must exist or every navigation/* query returns empty; call this after adding/moving an ANavMeshBoundsVolume, editing world geometry, or changing agent settings, before navigation/test_path or find_nearest_reachable_point. "
		     "Warning: blocking game-thread rebuild that FREEZES the editor and can take many minutes on large levels (the build kick waits up to 30 minutes before reporting a timeout); remaining_build_tasks may still be non-zero on return for async tiles and timed_out=true means tasks were still pending at the 300s poll cap."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("test_path"),
		TEXT("Compute a synchronous navmesh path between two world points (UNavigationSystemV1::FindPathToLocationSynchronously). "
		     "Returns {is_valid:bool, is_partial:bool, path_length:cm, waypoints:[{point:[X,Y,Z] cm}], waypoint_count, start, end}. "
		     "Params: start (number[3], required, world-space cm [X,Y,Z]), end (number[3], required, world-space cm [X,Y,Z]). "
		     "Workflow: build coverage with navigation/rebuild_navmesh first; snap loose endpoints with navigation/find_nearest_reachable_point. "
		     "Warning: read-only query but errors with null when there is no nav data; is_valid=false (or is_partial=true) when the points are off the navmesh -- a NavMeshBoundsVolume + built RecastNavMesh must cover both points."))
		.RequiredVec3(TEXT("start"), TEXT("Path start [X,Y,Z] world cm"))
		.RequiredVec3(TEXT("end"), TEXT("Path end [X,Y,Z] world cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("get_navmesh_bounds"),
		TEXT("List every ANavMeshBoundsVolume in the editor world with its bounds, plus a combined box spanning all of them. "
		     "Returns {volumes:[{label, origin, extent, min, max}], volume_count, combined_min, combined_max, combined_center, combined_extent} "
		     "(combined_* present only when at least one volume exists); all positions/extents in world-space cm. "
		     "Params: (none). "
		     "Workflow: read-only inspection of navmesh coverage before navigation/test_path or before placing AI; volume_count==0 means no nav coverage exists yet. "
		     "Warning: reports the bounds VOLUMES, not the generated RecastNavMesh tiles -- a volume can exist without a built navmesh, so still call navigation/rebuild_navmesh if queries return empty."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("find_nearest_reachable_point"),
		TEXT("Snap a world-space point onto the nearest navmesh location (UNavigationSystemV1::ProjectPointToNavigation). "
		     "Returns {found:bool, input_point, query_extent, projected_point (only when found), distance:cm (only when found)}. "
		     "Params: point (number[3], required, world-space cm [X,Y,Z]), "
		     "query_extent (number[3], optional, search-box HALF-extents in cm, default [500,500,500]). "
		     "Workflow: use to fix loose spawn locations or pathing endpoints before navigation/test_path; build coverage with navigation/rebuild_navmesh first. "
		     "Warning: read-only query; found=false (no projected_point) when nothing on the navmesh is within the search box -- widen query_extent if a known-good point fails."))
		.RequiredVec3(TEXT("point"), TEXT("World-space point to project [X,Y,Z] cm"))
		.OptionalVec3(TEXT("query_extent"), TEXT("Search box half-extents [X,Y,Z] cm (default [500,500,500])"))
		.Build());

	return Tools;
}

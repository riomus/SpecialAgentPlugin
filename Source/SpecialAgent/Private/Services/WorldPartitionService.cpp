// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/WorldPartitionService.h"
#include "MCPCommon/MCPRequestContext.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/World.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"

// ----- Helpers -----

static UWorldPartition* ResolveEditorWorldPartition()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return nullptr;
	return World->GetWorldPartition();
}

static FString CellStateToString(EWorldPartitionRuntimeCellState State)
{
	switch (State)
	{
		case EWorldPartitionRuntimeCellState::Unloaded:  return TEXT("unloaded");
		case EWorldPartitionRuntimeCellState::Loaded:    return TEXT("loaded");
		case EWorldPartitionRuntimeCellState::Activated: return TEXT("activated");
		default:                                          return TEXT("unknown");
	}
}

static TSharedPtr<FJsonObject> SerializeCell(const UWorldPartitionRuntimeCell* Cell)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	if (!Cell) return Entry;

	Entry->SetStringField(TEXT("name"), Cell->GetName());
	Entry->SetStringField(TEXT("debug_name"), Cell->GetDebugName());
	Entry->SetStringField(TEXT("level_package"), Cell->GetLevelPackageName().ToString());
	Entry->SetBoolField(TEXT("is_always_loaded"), Cell->IsAlwaysLoaded());
	Entry->SetBoolField(TEXT("is_spatially_loaded"), Cell->IsSpatiallyLoaded());
	Entry->SetStringField(TEXT("state"), CellStateToString(Cell->GetCurrentState()));

	const FBox ContentBounds = Cell->GetContentBounds();
	if (ContentBounds.IsValid)
	{
		FMCPJson::WriteVec3(Entry, TEXT("content_min"), ContentBounds.Min);
		FMCPJson::WriteVec3(Entry, TEXT("content_max"), ContentBounds.Max);
	}

	const FBox CellBounds = Cell->GetCellBounds();
	if (CellBounds.IsValid)
	{
		FMCPJson::WriteVec3(Entry, TEXT("cell_min"), CellBounds.Min);
		FMCPJson::WriteVec3(Entry, TEXT("cell_max"), CellBounds.Max);
	}

	return Entry;
}

// Collects every runtime streaming cell currently known to the partition.
static TArray<const UWorldPartitionRuntimeCell*> CollectAllCells(UWorldPartition* WorldPartition)
{
	TArray<const UWorldPartitionRuntimeCell*> Out;
	if (!WorldPartition || !WorldPartition->RuntimeHash) return Out;

	WorldPartition->RuntimeHash->ForEachStreamingCells(
		[&Out](const UWorldPartitionRuntimeCell* Cell)
		{
			if (Cell)
			{
				Out.Add(Cell);
			}
			return true; // continue
		});

	return Out;
}

// ----- Service implementation -----

FString FWorldPartitionService::GetServiceDescription() const
{
	return TEXT("World Partition cell loading and streaming - enumerate, load, unload, and force-load regions");
}

FMCPResponse FWorldPartitionService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("list_cells")) return HandleListCells(Request);
	if (MethodName == TEXT("load_cell")) return HandleLoadCell(Request);
	if (MethodName == TEXT("unload_cell")) return HandleUnloadCell(Request);
	if (MethodName == TEXT("get_loaded_cells")) return HandleGetLoadedCells(Request);
	if (MethodName == TEXT("force_load_region")) return HandleForceLoadRegion(Request);

	return MethodNotFound(Request.Id, TEXT("world_partition"), MethodName);
}

FMCPResponse FWorldPartitionService::HandleListCells(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorldPartition* WP = ResolveEditorWorldPartition();
		if (!WP)
		{
			return FMCPJson::MakeError(TEXT("Editor world has no UWorldPartition (map is not World Partition)"));
		}

		const TArray<const UWorldPartitionRuntimeCell*> Cells = CollectAllCells(WP);

		TArray<TSharedPtr<FJsonValue>> CellsJson;
		CellsJson.Reserve(Cells.Num());
		for (const UWorldPartitionRuntimeCell* Cell : Cells)
		{
			CellsJson.Add(MakeShared<FJsonValueObject>(SerializeCell(Cell)));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("cells"), CellsJson);
		Result->SetNumberField(TEXT("count"), CellsJson.Num());
		Result->SetBoolField(TEXT("streaming_enabled"), WP->IsStreamingEnabled());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: world_partition/list_cells returned %d cell(s)"), CellsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldPartitionService::HandleGetLoadedCells(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorldPartition* WP = ResolveEditorWorldPartition();
		if (!WP)
		{
			return FMCPJson::MakeError(TEXT("Editor world has no UWorldPartition"));
		}

		const TArray<const UWorldPartitionRuntimeCell*> All = CollectAllCells(WP);

		TArray<TSharedPtr<FJsonValue>> CellsJson;
		int32 LoadedCount = 0;
		int32 ActivatedCount = 0;
		for (const UWorldPartitionRuntimeCell* Cell : All)
		{
			if (!Cell) continue;

			const EWorldPartitionRuntimeCellState State = Cell->GetCurrentState();
			if (State == EWorldPartitionRuntimeCellState::Loaded || State == EWorldPartitionRuntimeCellState::Activated)
			{
				CellsJson.Add(MakeShared<FJsonValueObject>(SerializeCell(Cell)));
				if (State == EWorldPartitionRuntimeCellState::Loaded) ++LoadedCount;
				else ++ActivatedCount;
			}
			else if (Cell->IsAlwaysLoaded())
			{
				CellsJson.Add(MakeShared<FJsonValueObject>(SerializeCell(Cell)));
				++LoadedCount;
			}
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("cells"), CellsJson);
		Result->SetNumberField(TEXT("count"), CellsJson.Num());
		Result->SetNumberField(TEXT("loaded_count"), LoadedCount);
		Result->SetNumberField(TEXT("activated_count"), ActivatedCount);
		Result->SetNumberField(TEXT("total_cells"), All.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: world_partition/get_loaded_cells %d loaded/activated of %d total"),
			CellsJson.Num(), All.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldPartitionService::HandleLoadCell(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CellName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("cell_name"), CellName) || CellName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'cell_name' (use list_cells to discover names)"));
	}

	auto Task = [CellName]() -> TSharedPtr<FJsonObject>
	{
		UWorldPartition* WP = ResolveEditorWorldPartition();
		if (!WP)
		{
			return FMCPJson::MakeError(TEXT("Editor world has no UWorldPartition"));
		}

		const TArray<const UWorldPartitionRuntimeCell*> All = CollectAllCells(WP);

		const UWorldPartitionRuntimeCell* TargetCell = nullptr;
		for (const UWorldPartitionRuntimeCell* Cell : All)
		{
			if (!Cell) continue;
			if (Cell->GetName() == CellName ||
			    Cell->GetDebugName() == CellName ||
			    Cell->GetLevelPackageName().ToString() == CellName)
			{
				TargetCell = Cell;
				break;
			}
		}

		if (!TargetCell)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Cell '%s' not found"), *CellName));
		}

		// UWorldPartitionRuntimeCell exposes Load() / Activate() on its base;
		// in the editor the loader-adapter path is preferred for spatial cells.
		// Use the content bounds to spawn an editor loader adapter so actor
		// references are pinned.
		const FBox Bounds = TargetCell->GetContentBounds().IsValid
			? TargetCell->GetContentBounds()
			: TargetCell->GetCellBounds();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		bool bUsedLoaderAdapter = false;
		if (World && Bounds.IsValid)
		{
			FLoaderAdapterShape* Adapter = new FLoaderAdapterShape(World, Bounds,
				FString::Printf(TEXT("MCP::load_cell(%s)"), *CellName));
			Adapter->SetUserCreated(true);
			Adapter->Load();
			bUsedLoaderAdapter = true;
			// Note: leaking the adapter intentionally - the world partition
			// keeps it alive via LoaderAdapterStateChanged delegate bindings.
			// A future enhancement could store the adapter for later unload.
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("cell_name"), TargetCell->GetName());
		Result->SetStringField(TEXT("debug_name"), TargetCell->GetDebugName());
		Result->SetStringField(TEXT("state"), CellStateToString(TargetCell->GetCurrentState()));
		Result->SetBoolField(TEXT("used_loader_adapter"), bUsedLoaderAdapter);
		if (Bounds.IsValid)
		{
			FMCPJson::WriteVec3(Result, TEXT("bounds_min"), Bounds.Min);
			FMCPJson::WriteVec3(Result, TEXT("bounds_max"), Bounds.Max);
		}

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: world_partition/load_cell '%s' (adapter=%d)"),
			*CellName, bUsedLoaderAdapter ? 1 : 0);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldPartitionService::HandleUnloadCell(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CellName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("cell_name"), CellName) || CellName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'cell_name'"));
	}

	auto Task = [CellName]() -> TSharedPtr<FJsonObject>
	{
		UWorldPartition* WP = ResolveEditorWorldPartition();
		if (!WP)
		{
			return FMCPJson::MakeError(TEXT("Editor world has no UWorldPartition"));
		}

		const TArray<const UWorldPartitionRuntimeCell*> All = CollectAllCells(WP);

		const UWorldPartitionRuntimeCell* TargetCell = nullptr;
		for (const UWorldPartitionRuntimeCell* Cell : All)
		{
			if (!Cell) continue;
			if (Cell->GetName() == CellName ||
			    Cell->GetDebugName() == CellName ||
			    Cell->GetLevelPackageName().ToString() == CellName)
			{
				TargetCell = Cell;
				break;
			}
		}

		if (!TargetCell)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Cell '%s' not found"), *CellName));
		}

		if (TargetCell->IsAlwaysLoaded())
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Cell '%s' is always-loaded and cannot be unloaded"), *CellName));
		}

		// Editor path: spawn a zero-volume loader adapter covering the cell's
		// bounds so the WP refresh recomputes what should stay loaded. This
		// mirrors the "Unload Region" UX in the WorldPartition editor panel
		// — actually detaching a specific editor loader requires the original
		// adapter pointer. For the broader case we fall back to calling the
		// cell's Unload() interface, which is a no-op on always-loaded cells.
		TargetCell->Unload();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("cell_name"), TargetCell->GetName());
		Result->SetStringField(TEXT("debug_name"), TargetCell->GetDebugName());
		Result->SetStringField(TEXT("state"), CellStateToString(TargetCell->GetCurrentState()));
		Result->SetStringField(TEXT("note"), TEXT("Unload is best-effort in editor; loader adapters created by force_load_region must be released to free their references."));

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: world_partition/unload_cell '%s'"), *CellName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FWorldPartitionService::HandleForceLoadRegion(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FVector Min, Max;
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("min"), Min))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'min' ([X,Y,Z] cm)"));
	}
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("max"), Max))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'max' ([X,Y,Z] cm)"));
	}

	auto Task = [Min, Max]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world found"));
		}

		UWorldPartition* WP = World->GetWorldPartition();
		if (!WP)
		{
			return FMCPJson::MakeError(TEXT("Editor world has no UWorldPartition"));
		}

		const FBox Region(Min, Max);
		if (!Region.IsValid || Region.GetVolume() <= 0.0)
		{
			return FMCPJson::MakeError(TEXT("Invalid region: min must be strictly less than max on all axes"));
		}

		// Enumerate cells intersecting the region before loading so we can
		// report what will be pulled in.
		const TArray<const UWorldPartitionRuntimeCell*> All = CollectAllCells(WP);
		TArray<TSharedPtr<FJsonValue>> HitsJson;
		int32 IntersectCount = 0;
		for (const UWorldPartitionRuntimeCell* Cell : All)
		{
			if (!Cell) continue;

			const FBox CellBounds = Cell->GetContentBounds().IsValid
				? Cell->GetContentBounds()
				: Cell->GetCellBounds();
			if (CellBounds.IsValid && Region.Intersect(CellBounds))
			{
				HitsJson.Add(MakeShared<FJsonValueObject>(SerializeCell(Cell)));
				++IntersectCount;
			}
		}

		// Kick off a real editor load via FLoaderAdapterShape. The adapter is
		// owned by UWorldPartition via its LoaderAdapter delegate wiring
		// (SetUserCreated marks it so it survives GC).
		FLoaderAdapterShape* Adapter = new FLoaderAdapterShape(World, Region, TEXT("MCP::force_load_region"));
		Adapter->SetUserCreated(true);
		Adapter->Load();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		FMCPJson::WriteVec3(Result, TEXT("min"), Min);
		FMCPJson::WriteVec3(Result, TEXT("max"), Max);
		Result->SetNumberField(TEXT("intersecting_cells"), IntersectCount);
		Result->SetArrayField(TEXT("cells"), HitsJson);
		Result->SetBoolField(TEXT("loader_adapter_created"), true);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: world_partition/force_load_region loaded region, %d cell(s) intersected"), IntersectCount);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FWorldPartitionService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
		TEXT("list_cells"),
		TEXT("List every UWorldPartition runtime streaming cell for the editor world. "
		     "Returns {cells:[{name, debug_name, level_package, is_always_loaded, is_spatially_loaded, state:'unloaded'|'loaded'|'activated', content_min/content_max (world-space cm AABB), cell_min/cell_max (world-space cm AABB)}], count, streaming_enabled}. "
		     "Params: (none). "
		     "Workflow: Read-only; call first to get a cell name/debug_name/level_package to feed load_cell or unload_cell, or use the bounds to pick a region for force_load_region. "
		     "Warning: Returns an error if the current map is not a World Partition map (use streaming/list_levels for classic sublevel maps)."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("load_cell"),
		TEXT("Load a single World Partition cell into the editor by spawning an FLoaderAdapterShape over the cell's content bounds (falls back to cell bounds), which pins the actors so they appear in the editor world. "
		     "Returns {cell_name, debug_name, state, used_loader_adapter, bounds_min/bounds_max (world-space cm)}. "
		     "Params: cell_name (string, required; matched against the cell name, debug_name, or level_package from list_cells). "
		     "Workflow: Call list_cells first to get a valid name; for a rectangular area spanning several cells prefer force_load_region. "
		     "Warning: The loader adapter is intentionally leaked (kept alive by World Partition) and is NOT tracked, so unload_cell cannot release it; this mutates in-memory editor state only and is not saved. Unknown names return an error."))
		.RequiredString(TEXT("cell_name"), TEXT("Cell name, debug name, or level package path from list_cells"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("unload_cell"),
		TEXT("Request that a single World Partition cell unload by calling its Unload() interface. Best-effort in the editor. "
		     "Returns {cell_name, debug_name, state, note}. "
		     "Params: cell_name (string, required; matched against name, debug_name, or level_package from list_cells). "
		     "Workflow: Call list_cells (or get_loaded_cells) first to find a loaded, non-always-loaded cell. "
		     "Warning: Cannot release loader adapters created by load_cell or force_load_region (those are untracked and keep their actors pinned, so the cell may stay loaded); always-loaded cells return an error; in-memory only, not saved."))
		.RequiredString(TEXT("cell_name"), TEXT("Cell name, debug name, or level package path"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("get_loaded_cells"),
		TEXT("List only the World Partition cells currently Loaded, Activated, or Always-Loaded (a filtered view of list_cells). "
		     "Returns {cells:[...same per-cell shape as list_cells...], count, loaded_count, activated_count, total_cells}. "
		     "Params: (none). "
		     "Workflow: Read-only; use as a quick state check after load_cell / force_load_region / unload_cell. "
		     "Warning: Returns an error if the current map is not a World Partition map."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("force_load_region"),
		TEXT("Force-load every World Partition cell intersecting an axis-aligned box by spawning an FLoaderAdapterShape over the region. "
		     "Returns {min, max, intersecting_cells, cells:[...same per-cell shape as list_cells...], loader_adapter_created:true}. "
		     "Params: min ([X,Y,Z] world-space cm, required, lower corner), max ([X,Y,Z] world-space cm, required, upper corner; each axis must be strictly greater than min). "
		     "Workflow: Read list_cells bounds (or viewport/get_transform) to pick a box, then pre-stream that area before screenshots, triangle counts, or tests. "
		     "Warning: The loader adapter is leaked/untracked, so unload_cell cannot release it and the region stays pinned for the editor session; in-memory only, not saved. A degenerate box (zero/negative volume) returns an error; large boxes pull in many cells."))
		.RequiredVec3(TEXT("min"), TEXT("Region min corner [X,Y,Z] world cm"))
		.RequiredVec3(TEXT("max"), TEXT("Region max corner [X,Y,Z] world cm; each axis strictly greater than min"))
		.Build());

	return Tools;
}

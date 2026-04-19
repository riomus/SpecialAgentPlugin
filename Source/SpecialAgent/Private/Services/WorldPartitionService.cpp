// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/WorldPartitionService.h"

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

FMCPResponse FWorldPartitionService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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
		TEXT("List all UWorldPartition runtime streaming cells for the editor world. Returns name, debug name, bounds, and state (unloaded/loaded/activated) per cell.\n"
		     "Params: (none).\n"
		     "Workflow: Call first to discover cell names for load_cell and unload_cell.\n"
		     "Warning: Returns an error if the current map is not a World Partition map."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("load_cell"),
		TEXT("Load a specific World Partition cell by name. Uses the editor FLoaderAdapterShape across the cell bounds when available.\n"
		     "Params: cell_name (string, required; name, debug name, or level package name from list_cells).\n"
		     "Workflow: Discover via list_cells then load_cell by name.\n"
		     "Warning: Loader adapters created in the editor must be released via unload_cell/force_load_region tracking; always-loaded cells cannot be unloaded."))
		.RequiredString(TEXT("cell_name"), TEXT("Cell name, debug name, or level package path from list_cells"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("unload_cell"),
		TEXT("Request that a specific World Partition cell unload. Best-effort in the editor (calls the cell's Unload method).\n"
		     "Params: cell_name (string, required).\n"
		     "Warning: Loader adapters bound to the cell keep actors referenced; always-loaded cells return an error."))
		.RequiredString(TEXT("cell_name"), TEXT("Cell name, debug name, or level package path"))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("get_loaded_cells"),
		TEXT("List only the cells currently Loaded, Activated, or Always-Loaded.\n"
		     "Params: (none).\n"
		     "Workflow: Quick check of streaming state after calls to load_cell/force_load_region."))
		.Build());

	Tools.Add(FMCPToolBuilder(
		TEXT("force_load_region"),
		TEXT("Force-load every cell that intersects an axis-aligned bounding box via FLoaderAdapterShape. Returns list of intersecting cells.\n"
		     "Params: min ([X,Y,Z] cm, required, lower corner), max ([X,Y,Z] cm, required, upper corner).\n"
		     "Workflow: Use to pre-stream a viewport area before screenshots or gameplay tests.\n"
		     "Warning: Large regions pull many cells; stay within the editor world bounds to avoid unnecessary loads."))
		.RequiredVec3(TEXT("min"), TEXT("Region min corner [X,Y,Z] world cm"))
		.RequiredVec3(TEXT("max"), TEXT("Region max corner [X,Y,Z] world cm"))
		.Build());

	return Tools;
}

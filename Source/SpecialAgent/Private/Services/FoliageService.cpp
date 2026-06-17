// Copyright Epic Games, Inc. All Rights Reserved.
// FoliageService: direct C++ implementation for 5 foliage tools.

#include "Services/FoliageService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"

#include "Math/RandomStream.h"

FFoliageService::FFoliageService()
{
}

FString FFoliageService::GetServiceDescription() const
{
	return TEXT("Foliage management - paint, remove, and inspect instanced foliage");
}

FMCPResponse FFoliageService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("paint_in_area"))      return HandlePaintInArea(Request, Ctx);
	if (MethodName == TEXT("remove_from_area"))   return HandleRemoveFromArea(Request);
	if (MethodName == TEXT("get_density"))        return HandleGetDensity(Request);
	if (MethodName == TEXT("list_foliage_types")) return HandleListFoliageTypes(Request);
	if (MethodName == TEXT("add_foliage_type"))   return HandleAddFoliageType(Request);

	return MethodNotFound(Request.Id, TEXT("foliage"), MethodName);
}

// -----------------------------------------------------------------------------
// Common helpers
// -----------------------------------------------------------------------------

// Read a bounding box defined by "min"=[x,y,z] and "max"=[x,y,z]. Returns false if missing/invalid.
static bool ReadBox(const TSharedPtr<FJsonObject>& Params, FBox& OutBox)
{
	FVector Min, Max;
	if (!FMCPJson::ReadVec3(Params, TEXT("min"), Min)) return false;
	if (!FMCPJson::ReadVec3(Params, TEXT("max"), Max)) return false;
	OutBox = FBox(Min, Max);
	return OutBox.IsValid != 0;
}

static UFoliageType* LoadFoliageTypeByPath(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	return LoadObject<UFoliageType>(nullptr, *Path);
}

// -----------------------------------------------------------------------------
// paint_in_area
//   Params:
//     foliage_type  (string, asset path to UFoliageType)
//     min           (Vec3, world-space min corner of paint box)
//     max           (Vec3, world-space max corner of paint box)
//     count         (int, number of instances to place, required)
//     seed          (int, RNG seed, optional, default 0)
//
//   Places 'count' instances uniformly at random inside the box. Instances are
//   placed directly (no terrain trace); use raycast beforehand if ground-snap
//   needed.
// -----------------------------------------------------------------------------

FMCPResponse FFoliageService::HandlePaintInArea(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FoliageAssetPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("foliage_type"), FoliageAssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'foliage_type' (asset path)"));
	}

	FBox Box(ForceInitToZero);
	if (!ReadBox(Request.Params, Box))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'min'/'max' bounding box"));
	}

	int32 Count = 0;
	if (!FMCPJson::ReadInteger(Request.Params, TEXT("count"), Count) || Count <= 0)
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'count' (positive integer)"));
	}

	int32 Seed = 0;
	FMCPJson::ReadInteger(Request.Params, TEXT("seed"), Seed);

	auto SendProgress = Ctx.SendProgress;
	auto Task = [FoliageAssetPath, Box, Count, Seed, SendProgress]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		UFoliageType* FoliageType = LoadFoliageTypeByPath(FoliageAssetPath);
		if (!FoliageType)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: paint_in_area failed to load foliage type '%s'"), *FoliageAssetPath);
			return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UFoliageType: %s"), *FoliageAssetPath));
		}

		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone=*/ true);
		if (!IFA)
		{
			return FMCPJson::MakeError(TEXT("Could not obtain AInstancedFoliageActor"));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: paint foliage in area")));
		IFA->Modify();

		// Ensure the IFA knows about this foliage type.
		FFoliageInfo* Info = IFA->FindOrAddMesh(FoliageType);
		if (!Info)
		{
			return FMCPJson::MakeError(TEXT("FindOrAddMesh returned null for this foliage type"));
		}

		FRandomStream Rng(Seed);
		TArray<FFoliageInstance> Created;
		Created.Reserve(Count);

		for (int32 i = 0; i < Count; ++i)
		{
			FFoliageInstance Inst;
			Inst.Location = FVector(
				Rng.FRandRange(Box.Min.X, Box.Max.X),
				Rng.FRandRange(Box.Min.Y, Box.Max.Y),
				Rng.FRandRange(Box.Min.Z, Box.Max.Z));
			Inst.Rotation = FRotator(0.0f, Rng.FRandRange(0.0f, 360.0f), 0.0f);
			Inst.DrawScale3D = FVector3f(1.0f, 1.0f, 1.0f);
			Created.Add(Inst);
			if ((i + 1) % 100 == 0 || (i + 1) == Count)
				SendProgress((i + 1.0) / (double)Count, 1.0,
					FString::Printf(TEXT("paint_in_area %d/%d"), i + 1, Count));
		}

		// Add instances via the component-level helper on FFoliageInfo.
		TArray<const FFoliageInstance*> Ptrs;
		Ptrs.Reserve(Created.Num());
		for (const FFoliageInstance& Inst : Created)
		{
			Ptrs.Add(&Inst);
		}
		Info->AddInstances(FoliageType, Ptrs);
		IFA->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("foliage_type"), FoliageAssetPath);
		Result->SetNumberField(TEXT("placed"), Created.Num());
		FMCPJson::WriteVec3(Result, TEXT("min"), Box.Min);
		FMCPJson::WriteVec3(Result, TEXT("max"), Box.Max);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: paint_in_area placed %d instances of %s"),
			Created.Num(), *FoliageAssetPath);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// remove_from_area
//   Params:
//     min, max      (Vec3, world-space bounding box)
//     foliage_type  (string, optional asset path; if omitted, removes all types)
// -----------------------------------------------------------------------------

FMCPResponse FFoliageService::HandleRemoveFromArea(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FBox Box(ForceInitToZero);
	if (!ReadBox(Request.Params, Box))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'min'/'max' bounding box"));
	}

	FString FoliageAssetPath;
	FMCPJson::ReadString(Request.Params, TEXT("foliage_type"), FoliageAssetPath);

	auto Task = [Box, FoliageAssetPath]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone=*/ false);
		if (!IFA)
		{
			// Nothing painted yet -> treat as success with 0 removed.
			TSharedPtr<FJsonObject> Empty = FMCPJson::MakeSuccess();
			Empty->SetNumberField(TEXT("removed"), 0);
			return Empty;
		}

		UFoliageType* Filter = nullptr;
		if (!FoliageAssetPath.IsEmpty())
		{
			Filter = LoadFoliageTypeByPath(FoliageAssetPath);
			if (!Filter)
			{
				return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UFoliageType: %s"), *FoliageAssetPath));
			}
		}

		int32 TotalRemoved = 0;

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: remove foliage from area")));
		IFA->Modify();

		IFA->ForEachFoliageInfo([&](UFoliageType* Type, FFoliageInfo& Info) -> bool
		{
			if (Filter && Type != Filter)
			{
				return true; // skip, continue iteration
			}

			TArray<int32> InBox;
			Info.GetInstancesInsideBounds(Box, InBox);
			if (InBox.Num() > 0)
			{
				Info.RemoveInstances(InBox, /*RebuildFoliageTree=*/ true);
				TotalRemoved += InBox.Num();
			}
			return true;
		});

		IFA->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("removed"), TotalRemoved);
		if (!FoliageAssetPath.IsEmpty())
		{
			Result->SetStringField(TEXT("foliage_type"), FoliageAssetPath);
		}
		FMCPJson::WriteVec3(Result, TEXT("min"), Box.Min);
		FMCPJson::WriteVec3(Result, TEXT("max"), Box.Max);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: remove_from_area removed %d instances"), TotalRemoved);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// get_density
//   Counts instances inside a bounding box and reports instances per square cm
//   (using the XY footprint of the box). Useful for verifying paint strength.
//
//   Params: min, max (Vec3), foliage_type (string, optional).
// -----------------------------------------------------------------------------

FMCPResponse FFoliageService::HandleGetDensity(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FBox Box(ForceInitToZero);
	if (!ReadBox(Request.Params, Box))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'min'/'max' bounding box"));
	}

	FString FoliageAssetPath;
	FMCPJson::ReadString(Request.Params, TEXT("foliage_type"), FoliageAssetPath);

	auto Task = [Box, FoliageAssetPath]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone=*/ false);
		int32 TotalCount = 0;

		if (IFA)
		{
			UFoliageType* Filter = nullptr;
			if (!FoliageAssetPath.IsEmpty())
			{
				Filter = LoadFoliageTypeByPath(FoliageAssetPath);
				if (!Filter)
				{
					return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UFoliageType: %s"), *FoliageAssetPath));
				}
				TotalCount = IFA->GetOverlappingBoxCount(Filter, Box);
			}
			else
			{
				IFA->ForEachFoliageInfo([&](UFoliageType* Type, FFoliageInfo& Info) -> bool
				{
					TArray<int32> InBox;
					Info.GetInstancesInsideBounds(Box, InBox);
					TotalCount += InBox.Num();
					return true;
				});
			}
		}

		const FVector Size = Box.GetSize();
		const double AreaXY = FMath::Max<double>(FMath::Abs(Size.X * Size.Y), 1e-6);
		const double Density = static_cast<double>(TotalCount) / AreaXY; // instances per cm^2

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("count"), TotalCount);
		Result->SetNumberField(TEXT("area_xy_cm2"), AreaXY);
		Result->SetNumberField(TEXT("density_per_cm2"), Density);
		if (!FoliageAssetPath.IsEmpty())
		{
			Result->SetStringField(TEXT("foliage_type"), FoliageAssetPath);
		}
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: get_density count=%d area_xy=%.1f"), TotalCount, AreaXY);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// list_foliage_types
//   Enumerate UFoliageType entries currently registered on the level's IFA.
// -----------------------------------------------------------------------------

FMCPResponse FFoliageService::HandleListFoliageTypes(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TArray<TSharedPtr<FJsonValue>> TypesArr;

		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone=*/ false);
		if (IFA)
		{
			IFA->ForEachFoliageInfo([&](UFoliageType* Type, FFoliageInfo& Info) -> bool
			{
				if (!Type) return true;
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset_path"), Type->GetPathName());
				Entry->SetStringField(TEXT("name"),       Type->GetName());
				Entry->SetStringField(TEXT("class"),      Type->GetClass()->GetName());
				Entry->SetNumberField(TEXT("instance_count"), Info.Instances.Num());
				TypesArr.Add(MakeShared<FJsonValueObject>(Entry));
				return true;
			});
		}

		Result->SetArrayField(TEXT("foliage_types"), TypesArr);
		Result->SetNumberField(TEXT("count"), TypesArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: list_foliage_types found %d"), TypesArr.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// add_foliage_type
//   Load a UFoliageType asset from path and register it on the current IFA,
//   so subsequent paint_in_area calls can place instances without first
//   touching AddInstances.
// -----------------------------------------------------------------------------

FMCPResponse FFoliageService::HandleAddFoliageType(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FoliageAssetPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("foliage_type"), FoliageAssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'foliage_type' (asset path)"));
	}

	auto Task = [FoliageAssetPath]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		UFoliageType* FoliageType = LoadFoliageTypeByPath(FoliageAssetPath);
		if (!FoliageType)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: add_foliage_type failed to load '%s'"), *FoliageAssetPath);
			return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UFoliageType: %s"), *FoliageAssetPath));
		}

		AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone=*/ true);
		if (!IFA)
		{
			return FMCPJson::MakeError(TEXT("Could not obtain AInstancedFoliageActor"));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add foliage type")));
		IFA->Modify();

		FFoliageInfo* Info = IFA->FindOrAddMesh(FoliageType);
		if (!Info)
		{
			return FMCPJson::MakeError(TEXT("FindOrAddMesh returned null"));
		}
		IFA->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("foliage_type"), FoliageAssetPath);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: add_foliage_type registered '%s'"), *FoliageAssetPath);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// Tool catalog
// -----------------------------------------------------------------------------

TArray<FMCPToolInfo> FFoliageService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
			TEXT("paint_in_area"),
			TEXT("Scatter instanced foliage uniformly at random inside a world-space axis-aligned box. "
			     "Each instance gets a random position inside the box (including a random Z within [min.Z, max.Z]), a random yaw, and unit scale. "
			     "Returns {success, foliage_type, placed (int actually added), min, max}.\n"
			     "Params: foliage_type (string, virtual asset path to a UFoliageType e.g. /Game/Foliage/FT_Grass, required), "
			     "min ([X,Y,Z] world-space cm, required), max ([X,Y,Z] world-space cm, required), "
			     "count (int, number of instances, must be > 0, required), seed (int, RNG seed, optional, default 0).\n"
			     "Workflow: pass a UFoliageType asset, not a raw StaticMesh (the type wraps mesh + density/scale/cull). "
			     "This tool auto-registers the type via InstancedFoliageActor.FindOrAddMesh, so foliage/add_foliage_type is optional. "
			     "Verify results with foliage/get_density or foliage/list_foliage_types.\n"
			     "Warning: placement does NOT trace to the ground -- Z is random within the box, so for terrain-snapped paint trace each point first and pass a flat box. "
			     "Mutates the level's InstancedFoliageActor in memory only; save the level to persist. The same seed reproduces the same layout."))
		.RequiredString (TEXT("foliage_type"), TEXT("Virtual asset path to a UFoliageType (e.g. /Game/Foliage/FT_Grass)"))
		.RequiredVec3   (TEXT("min"),          TEXT("Box min corner [X,Y,Z] in world-space cm"))
		.RequiredVec3   (TEXT("max"),          TEXT("Box max corner [X,Y,Z] in world-space cm"))
		.RequiredInteger(TEXT("count"),        TEXT("Number of instances to place (must be > 0)"))
		.OptionalInteger(TEXT("seed"),         TEXT("RNG seed for deterministic placement (default 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("remove_from_area"),
			TEXT("Delete foliage instances whose origin falls inside a world-space axis-aligned box. "
			     "When foliage_type is omitted, every registered type in the box is removed; when supplied, only that type. "
			     "Returns {success, removed (int instance count), foliage_type (only if supplied), min, max}.\n"
			     "Params: min ([X,Y,Z] world-space cm, required), max ([X,Y,Z] world-space cm, required), "
			     "foliage_type (string, virtual asset path to a UFoliageType, optional -- omit to remove all types).\n"
			     "Workflow: call foliage/list_foliage_types first to discover registered type paths before a targeted removal.\n"
			     "Warning: deletes instances immediately and rebuilds the foliage tree; wrapped in an undo transaction so it is reversible (Ctrl+Z). "
			     "If nothing has been painted yet (no InstancedFoliageActor) it is a safe no-op returning removed=0. Save the level to persist."))
		.RequiredVec3  (TEXT("min"),          TEXT("Box min corner [X,Y,Z] in world-space cm"))
		.RequiredVec3  (TEXT("max"),          TEXT("Box max corner [X,Y,Z] in world-space cm"))
		.OptionalString(TEXT("foliage_type"), TEXT("Virtual asset path to a UFoliageType; omit to remove all types"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_density"),
			TEXT("Count foliage instances inside a world-space box and report density over the box's XY footprint. "
			     "When foliage_type is omitted, all types are counted; when supplied, only that type. "
			     "Returns {success, count (int), area_xy_cm2 (number, |sizeX*sizeY|), density_per_cm2 (count/area), foliage_type (only if supplied)}.\n"
			     "Params: min ([X,Y,Z] world-space cm, required), max ([X,Y,Z] world-space cm, required), "
			     "foliage_type (string, virtual asset path to a UFoliageType, optional -- omit to count all types).\n"
			     "Workflow: read-only; use to verify paint_in_area / remove_from_area output. Density uses only the XY area, so box Z thickness does not affect it.\n"
			     "Warning: returns count=0 (not an error) when no InstancedFoliageActor exists yet. density_per_cm2 is per square centimetre, so values are tiny -- multiply by 10000 for per-square-metre."))
		.RequiredVec3  (TEXT("min"),          TEXT("Box min corner [X,Y,Z] in world-space cm"))
		.RequiredVec3  (TEXT("max"),          TEXT("Box max corner [X,Y,Z] in world-space cm"))
		.OptionalString(TEXT("foliage_type"), TEXT("Virtual asset path to a UFoliageType; omit to count all types"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("list_foliage_types"),
			TEXT("List every UFoliageType currently registered on the level's InstancedFoliageActor, with live instance counts. "
			     "Returns {success, count (int, number of types), foliage_types: [{asset_path (virtual path, feed to other foliage tools), name, class, instance_count}]}.\n"
			     "Params: (none).\n"
			     "Workflow: read-only; call before paint_in_area / remove_from_area / get_density to confirm which type paths exist.\n"
			     "Warning: returns an empty list (count=0) when nothing has been painted yet -- the InstancedFoliageActor is created lazily on first paint."))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("add_foliage_type"),
			TEXT("Load a UFoliageType asset by path and register it on the level's InstancedFoliageActor (creating that actor if needed) without placing any instances. "
			     "Returns {success, foliage_type (the registered path)}.\n"
			     "Params: foliage_type (string, virtual asset path to a UFoliageType e.g. /Game/Foliage/FT_Grass, required).\n"
			     "Workflow: optional -- paint_in_area already auto-registers the type. Use this to pre-register a type so it shows in list_foliage_types before any paint.\n"
			     "Warning: idempotent -- re-registering an already-registered type is a safe no-op. Errors if the path cannot be loaded as a UFoliageType. Save the level to persist the registration."))
		.RequiredString(TEXT("foliage_type"), TEXT("Virtual asset path to a UFoliageType (e.g. /Game/Foliage/FT_Grass)"))
		.Build());

	return Tools;
}

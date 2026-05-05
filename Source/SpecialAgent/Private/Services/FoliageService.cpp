// Copyright Epic Games, Inc. All Rights Reserved.
// FoliageService: direct C++ implementation for 5 foliage tools.

#include "Services/FoliageService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/World.h"

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

		FFoliageInfo* Info = IFA->FindOrAddMesh(FoliageType);
		if (!Info)
		{
			return FMCPJson::MakeError(TEXT("FindOrAddMesh returned null"));
		}

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
			TEXT("Paint instanced foliage inside a world-space bounding box. Uniform random placement; Yaw is randomized, Z is taken from box (no ground trace).\n"
			     "Params: foliage_type (string, asset path to UFoliageType), min ([X,Y,Z] cm), max ([X,Y,Z] cm), count (int, instances to place), seed (int, RNG seed, optional).\n"
			     "Workflow: Call foliage/add_foliage_type first (optional), then this tool.\n"
			     "Warning: No terrain snap. For ground-snapped paint, raycast per instance separately."))
		.RequiredString (TEXT("foliage_type"), TEXT("Asset path to UFoliageType"))
		.RequiredVec3   (TEXT("min"),          TEXT("Bounding box min corner [X,Y,Z] in cm"))
		.RequiredVec3   (TEXT("max"),          TEXT("Bounding box max corner [X,Y,Z] in cm"))
		.RequiredInteger(TEXT("count"),        TEXT("Number of instances to place"))
		.OptionalInteger(TEXT("seed"),         TEXT("RNG seed (default 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("remove_from_area"),
			TEXT("Remove foliage instances inside a bounding box. If foliage_type omitted, removes all types.\n"
			     "Params: min ([X,Y,Z] cm, required), max ([X,Y,Z] cm, required), foliage_type (string, asset path, optional).\n"
			     "Workflow: pair with foliage/list_foliage_types to discover registered types before targeted removal.\n"
			     "Warning: irreversible without an open undo transaction; wrap in utility/begin_transaction first."))
		.RequiredVec3  (TEXT("min"),          TEXT("Bounding box min corner"))
		.RequiredVec3  (TEXT("max"),          TEXT("Bounding box max corner"))
		.OptionalString(TEXT("foliage_type"), TEXT("Asset path; when omitted, removes all types"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_density"),
			TEXT("Count foliage instances inside a bounding box and report density per XY cm^2.\n"
			     "Params: min, max ([X,Y,Z] cm), foliage_type (string, optional).\n"
			     "Workflow: Use to verify paint_in_area output."))
		.RequiredVec3  (TEXT("min"),          TEXT("Bounding box min corner"))
		.RequiredVec3  (TEXT("max"),          TEXT("Bounding box max corner"))
		.OptionalString(TEXT("foliage_type"), TEXT("Asset path to restrict count"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("list_foliage_types"),
			TEXT("Enumerate UFoliageType entries currently registered on the level's InstancedFoliageActor.\n"
			     "Params: (none).\n"
			     "Returns: array of {asset_path, name, class, instance_count}.\n"
			     "Workflow: call before paint_in_area / remove_from_area to confirm types are registered."))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("add_foliage_type"),
			TEXT("Load a UFoliageType asset by path and register it on the level's InstancedFoliageActor. Idempotent.\n"
			     "Params: foliage_type (string, required, asset path).\n"
			     "Workflow: must precede paint_in_area for that type; pair with list_foliage_types to verify.\n"
			     "Warning: re-registering an already-registered type is a no-op (safe)."))
		.RequiredString(TEXT("foliage_type"), TEXT("Asset path to UFoliageType"))
		.Build());

	return Tools;
}

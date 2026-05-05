// Copyright Epic Games, Inc. All Rights Reserved.
// LandscapeService: direct C++ implementation for 6 landscape tools.

#include "Services/LandscapeService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeComponent.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeEdit.h"

#include "Materials/MaterialInterface.h"

FLandscapeService::FLandscapeService()
{
}

FString FLandscapeService::GetServiceDescription() const
{
	return TEXT("Landscape terrain editing - sculpt, flatten, smooth, and paint layers");
}

FMCPResponse FLandscapeService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("get_info"))      return HandleGetInfo(Request);
	if (MethodName == TEXT("sculpt_height")) return HandleSculptHeight(Request);
	if (MethodName == TEXT("flatten_area"))  return HandleFlattenArea(Request);
	if (MethodName == TEXT("smooth_area"))   return HandleSmoothArea(Request, Ctx);
	if (MethodName == TEXT("paint_layer"))   return HandlePaintLayer(Request);
	if (MethodName == TEXT("list_layers"))   return HandleListLayers(Request);

	return MethodNotFound(Request.Id, TEXT("landscape"), MethodName);
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

// Find the first ALandscape actor in the editor world.
static ALandscape* FindLandscape(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

// Height conversion:
//   Landscape raw height is stored as uint16. 32768 is "zero". 1 uint16 unit == LANDSCAPE_ZSCALE cm
//   (see LandscapeDataAccess.h). We want a round-trip in cm.
static constexpr double LandscapeZScaleCm = 1.0 / 128.0; // UE convention: 128 units per cm.

static FORCEINLINE uint16 HeightCmToRaw(double HeightCm)
{
	const double Raw = (HeightCm / LandscapeZScaleCm) + 32768.0;
	return static_cast<uint16>(FMath::Clamp(Raw, 0.0, 65535.0));
}

static FORCEINLINE double RawToHeightCm(uint16 Raw)
{
	return (static_cast<double>(Raw) - 32768.0) * LandscapeZScaleCm;
}

// Read a rect in landscape quad coordinates: x1, y1, x2, y2 (required).
static bool ReadRect(const TSharedPtr<FJsonObject>& Params, int32& X1, int32& Y1, int32& X2, int32& Y2)
{
	if (!FMCPJson::ReadInteger(Params, TEXT("x1"), X1)) return false;
	if (!FMCPJson::ReadInteger(Params, TEXT("y1"), Y1)) return false;
	if (!FMCPJson::ReadInteger(Params, TEXT("x2"), X2)) return false;
	if (!FMCPJson::ReadInteger(Params, TEXT("y2"), Y2)) return false;
	if (X2 < X1) Swap(X1, X2);
	if (Y2 < Y1) Swap(Y1, Y2);
	return true;
}

// -----------------------------------------------------------------------------
// get_info
// -----------------------------------------------------------------------------

FMCPResponse FLandscapeService::HandleGetInfo(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		ALandscape* Landscape = FindLandscape(World);
		if (!Landscape)
		{
			return FMCPJson::MakeError(TEXT("No ALandscape found in level"));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), Landscape->GetActorLabel());
		Result->SetNumberField(TEXT("component_size_quads"), Landscape->ComponentSizeQuads);
		Result->SetNumberField(TEXT("subsection_size_quads"), Landscape->SubsectionSizeQuads);
		Result->SetNumberField(TEXT("num_subsections"), Landscape->NumSubsections);

		if (Landscape->LandscapeMaterial)
		{
			Result->SetStringField(TEXT("material_path"), Landscape->LandscapeMaterial->GetPathName());
		}

		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (Info)
		{
			int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
			if (Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				Result->SetNumberField(TEXT("min_x"), MinX);
				Result->SetNumberField(TEXT("min_y"), MinY);
				Result->SetNumberField(TEXT("max_x"), MaxX);
				Result->SetNumberField(TEXT("max_y"), MaxY);
				Result->SetNumberField(TEXT("size_x_quads"), MaxX - MinX);
				Result->SetNumberField(TEXT("size_y_quads"), MaxY - MinY);
			}

			int32 ComponentCount = 0;
			Info->ForAllLandscapeComponents([&ComponentCount](ULandscapeComponent*) { ComponentCount++; });
			Result->SetNumberField(TEXT("component_count"), ComponentCount);
		}

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: landscape/get_info '%s'"), *Landscape->GetActorLabel());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// sculpt_height
//   Params: x1,y1,x2,y2 (int quad coords), delta_cm (number, +/- height delta)
//   Adds delta_cm (after converting to raw units) to every sample in the rect.
// -----------------------------------------------------------------------------

FMCPResponse FLandscapeService::HandleSculptHeight(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
	if (!ReadRect(Request.Params, X1, Y1, X2, Y2))
	{
		return InvalidParams(Request.Id, TEXT("Missing rect (x1,y1,x2,y2) in landscape quad coords"));
	}
	double DeltaCm = 0.0;
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("delta_cm"), DeltaCm))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'delta_cm' (number)"));
	}

	auto Task = [X1, Y1, X2, Y2, DeltaCm]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		ALandscape* Landscape = FindLandscape(World);
		if (!Landscape) return FMCPJson::MakeError(TEXT("No ALandscape found"));
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info) return FMCPJson::MakeError(TEXT("Landscape has no ULandscapeInfo"));

		const int32 Width = X2 - X1 + 1;
		const int32 Height = Y2 - Y1 + 1;
		const int32 Num = Width * Height;
		if (Num <= 0) return FMCPJson::MakeError(TEXT("Empty rect"));

		TArray<uint16> Data;
		Data.SetNumZeroed(Num);

		FLandscapeEditDataInterface EditInterface(Info);
		int32 RX1 = X1, RY1 = Y1, RX2 = X2, RY2 = Y2;
		EditInterface.GetHeightData(RX1, RY1, RX2, RY2, Data.GetData(), Width);

		// Apply uniform delta in raw units.
		const double DeltaRawD = DeltaCm / LandscapeZScaleCm;
		const int32 DeltaRaw = FMath::RoundToInt(DeltaRawD);

		for (int32 i = 0; i < Num; ++i)
		{
			const int32 NewVal = static_cast<int32>(Data[i]) + DeltaRaw;
			Data[i] = static_cast<uint16>(FMath::Clamp(NewVal, 0, 65535));
		}

		EditInterface.SetHeightData(X1, Y1, X2, Y2, Data.GetData(), Width, /*InCalcNormals=*/ true);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("samples"), Num);
		Result->SetNumberField(TEXT("delta_cm"), DeltaCm);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: sculpt_height rect=(%d,%d)-(%d,%d) delta_cm=%.1f samples=%d"),
			X1, Y1, X2, Y2, DeltaCm, Num);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// flatten_area
//   Set every sample in rect to target_z_cm (world cm).
// -----------------------------------------------------------------------------

FMCPResponse FLandscapeService::HandleFlattenArea(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
	if (!ReadRect(Request.Params, X1, Y1, X2, Y2))
	{
		return InvalidParams(Request.Id, TEXT("Missing rect (x1,y1,x2,y2)"));
	}
	double TargetZCm = 0.0;
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("target_z_cm"), TargetZCm))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'target_z_cm' (number)"));
	}

	auto Task = [X1, Y1, X2, Y2, TargetZCm]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		ALandscape* Landscape = FindLandscape(World);
		if (!Landscape) return FMCPJson::MakeError(TEXT("No ALandscape found"));
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info) return FMCPJson::MakeError(TEXT("Landscape has no ULandscapeInfo"));

		const int32 Width = X2 - X1 + 1;
		const int32 Height = Y2 - Y1 + 1;
		const int32 Num = Width * Height;
		if (Num <= 0) return FMCPJson::MakeError(TEXT("Empty rect"));

		// Convert using landscape transform to compensate for actor Z.
		const FTransform LocalToWorld = Landscape->LandscapeActorToWorld();
		const double LocalZCm = (TargetZCm - LocalToWorld.GetLocation().Z) / LocalToWorld.GetScale3D().Z;
		const uint16 Raw = HeightCmToRaw(LocalZCm);

		TArray<uint16> Data;
		Data.Init(Raw, Num);

		FLandscapeEditDataInterface EditInterface(Info);
		EditInterface.SetHeightData(X1, Y1, X2, Y2, Data.GetData(), Width, /*InCalcNormals=*/ true);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("samples"), Num);
		Result->SetNumberField(TEXT("target_z_cm"), TargetZCm);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: flatten_area rect=(%d,%d)-(%d,%d) z=%.1f samples=%d"),
			X1, Y1, X2, Y2, TargetZCm, Num);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// smooth_area
//   Apply a 3x3 box-average filter N times (iterations, default 1).
// -----------------------------------------------------------------------------

FMCPResponse FLandscapeService::HandleSmoothArea(const FMCPRequest& Request, const FMCPRequestContext& Ctx)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
	if (!ReadRect(Request.Params, X1, Y1, X2, Y2))
	{
		return InvalidParams(Request.Id, TEXT("Missing rect (x1,y1,x2,y2)"));
	}
	int32 Iterations = 1;
	FMCPJson::ReadInteger(Request.Params, TEXT("iterations"), Iterations);
	Iterations = FMath::Clamp(Iterations, 1, 16);

	auto SendProgress = Ctx.SendProgress;
	auto Task = [X1, Y1, X2, Y2, Iterations, SendProgress]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		ALandscape* Landscape = FindLandscape(World);
		if (!Landscape) return FMCPJson::MakeError(TEXT("No ALandscape found"));
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info) return FMCPJson::MakeError(TEXT("Landscape has no ULandscapeInfo"));

		const int32 Width = X2 - X1 + 1;
		const int32 Height = Y2 - Y1 + 1;
		const int32 Num = Width * Height;
		if (Num <= 0 || Width < 3 || Height < 3)
		{
			return FMCPJson::MakeError(TEXT("Rect must be at least 3x3 for smoothing"));
		}

		TArray<uint16> Data;
		Data.SetNumZeroed(Num);

		FLandscapeEditDataInterface EditInterface(Info);
		int32 RX1 = X1, RY1 = Y1, RX2 = X2, RY2 = Y2;
		EditInterface.GetHeightData(RX1, RY1, RX2, RY2, Data.GetData(), Width);

		TArray<uint16> Temp;
		Temp.SetNumZeroed(Num);

		auto Idx = [Width](int32 Xi, int32 Yi) { return Yi * Width + Xi; };

		for (int32 Iter = 0; Iter < Iterations; ++Iter)
		{
			// Copy borders straight through, smooth interior.
			for (int32 y = 0; y < Height; ++y)
			{
				for (int32 x = 0; x < Width; ++x)
				{
					if (x == 0 || y == 0 || x == Width - 1 || y == Height - 1)
					{
						Temp[Idx(x, y)] = Data[Idx(x, y)];
					}
					else
					{
						int32 Sum = 0;
						for (int32 dy = -1; dy <= 1; ++dy)
						{
							for (int32 dx = -1; dx <= 1; ++dx)
							{
								Sum += Data[Idx(x + dx, y + dy)];
							}
						}
						Temp[Idx(x, y)] = static_cast<uint16>(Sum / 9);
					}
				}
			}
			Swap(Data, Temp);
			SendProgress((Iter + 1.0) / (double)Iterations, 1.0,
				FString::Printf(TEXT("smooth_area pass %d/%d"), Iter + 1, Iterations));
		}

		EditInterface.SetHeightData(X1, Y1, X2, Y2, Data.GetData(), Width, /*InCalcNormals=*/ true);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetNumberField(TEXT("samples"), Num);
		Result->SetNumberField(TEXT("iterations"), Iterations);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: smooth_area rect=(%d,%d)-(%d,%d) iters=%d"), X1, Y1, X2, Y2, Iterations);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// paint_layer
//   Params: x1..y2 (int quad coords), layer_name (string), alpha (number 0-1).
//   Fills the region with a uniform 8-bit alpha for the given layer.
// -----------------------------------------------------------------------------

FMCPResponse FLandscapeService::HandlePaintLayer(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
	if (!ReadRect(Request.Params, X1, Y1, X2, Y2))
	{
		return InvalidParams(Request.Id, TEXT("Missing rect (x1,y1,x2,y2)"));
	}
	FString LayerName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("layer_name"), LayerName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'layer_name'"));
	}
	double Alpha = 1.0;
	FMCPJson::ReadNumber(Request.Params, TEXT("alpha"), Alpha);
	Alpha = FMath::Clamp(Alpha, 0.0, 1.0);

	auto Task = [X1, Y1, X2, Y2, LayerName, Alpha]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		ALandscape* Landscape = FindLandscape(World);
		if (!Landscape) return FMCPJson::MakeError(TEXT("No ALandscape found"));
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info) return FMCPJson::MakeError(TEXT("Landscape has no ULandscapeInfo"));

		ULandscapeLayerInfoObject* LayerInfo = Info->GetLayerInfoByName(FName(*LayerName), Landscape);
		if (!LayerInfo)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: paint_layer layer not found '%s'"), *LayerName);
			return FMCPJson::MakeError(FString::Printf(TEXT("Layer not found: %s"), *LayerName));
		}

		const int32 Width = X2 - X1 + 1;
		const int32 Height = Y2 - Y1 + 1;
		const int32 Num = Width * Height;
		if (Num <= 0) return FMCPJson::MakeError(TEXT("Empty rect"));

		const uint8 AlphaByte = static_cast<uint8>(FMath::RoundToInt(Alpha * 255.0));
		TArray<uint8> Data;
		Data.Init(AlphaByte, Num);

		FLandscapeEditDataInterface EditInterface(Info);
		EditInterface.SetAlphaData(LayerInfo, X1, Y1, X2, Y2, Data.GetData(), Width);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("layer_name"), LayerName);
		Result->SetNumberField(TEXT("alpha"), Alpha);
		Result->SetNumberField(TEXT("samples"), Num);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: paint_layer '%s' alpha=%.2f rect=(%d,%d)-(%d,%d)"),
			*LayerName, Alpha, X1, Y1, X2, Y2);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// list_layers
// -----------------------------------------------------------------------------

FMCPResponse FLandscapeService::HandleListLayers(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		ALandscape* Landscape = FindLandscape(World);
		if (!Landscape) return FMCPJson::MakeError(TEXT("No ALandscape found"));
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info) return FMCPJson::MakeError(TEXT("Landscape has no ULandscapeInfo"));

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TArray<TSharedPtr<FJsonValue>> LayersArr;

		for (const FLandscapeInfoLayerSettings& L : Info->Layers)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("layer_name"), L.GetLayerName().ToString());
			if (L.LayerInfoObj)
			{
				Entry->SetStringField(TEXT("asset_path"), L.LayerInfoObj->GetPathName());
				Entry->SetBoolField(TEXT("has_info"), true);
			}
			else
			{
				Entry->SetBoolField(TEXT("has_info"), false);
			}
			LayersArr.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Result->SetArrayField(TEXT("layers"), LayersArr);
		Result->SetNumberField(TEXT("count"), LayersArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: landscape/list_layers %d layers"), LayersArr.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// Tool catalog
// -----------------------------------------------------------------------------

TArray<FMCPToolInfo> FLandscapeService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
			TEXT("get_info"),
			TEXT("Report landscape dimensions, component counts, and material.\n"
			     "Params: (none).\n"
			     "Returns: {actor_name, component_size_quads, subsection_size_quads, num_subsections, min_x, min_y, max_x, max_y, size_x_quads, size_y_quads, component_count, material_path}.\n"
			     "Workflow: call first to discover valid quad extents before sculpt_height / flatten_area / paint_layer."))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("sculpt_height"),
			TEXT("Raise or lower landscape heights across a rectangular region by a constant delta (cm).\n"
			     "Params: x1,y1,x2,y2 (int, landscape quad coordinates), delta_cm (number, positive = raise).\n"
			     "Workflow: Call landscape/get_info first to see valid min/max quad extents.\n"
			     "Warning: Only sculpts the active edit layer. Runtime regeneration may delay visible change."))
		.RequiredInteger(TEXT("x1"),       TEXT("Min X in landscape quad coords"))
		.RequiredInteger(TEXT("y1"),       TEXT("Min Y in landscape quad coords"))
		.RequiredInteger(TEXT("x2"),       TEXT("Max X in landscape quad coords"))
		.RequiredInteger(TEXT("y2"),       TEXT("Max Y in landscape quad coords"))
		.RequiredNumber (TEXT("delta_cm"), TEXT("Height delta in cm (positive to raise)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("flatten_area"),
			TEXT("Flatten a rectangular landscape region to a target world-Z height (cm).\n"
			     "Params: x1,y1,x2,y2 (int quad coords, required), target_z_cm (number, world cm, required).\n"
			     "Workflow: pair with landscape/get_info to find valid quad extents.\n"
			     "Warning: only mutates the active edit layer; runtime regeneration may delay visible change."))
		.RequiredInteger(TEXT("x1"),          TEXT("Min X in quad coords"))
		.RequiredInteger(TEXT("y1"),          TEXT("Min Y in quad coords"))
		.RequiredInteger(TEXT("x2"),          TEXT("Max X in quad coords"))
		.RequiredInteger(TEXT("y2"),          TEXT("Max Y in quad coords"))
		.RequiredNumber (TEXT("target_z_cm"), TEXT("World-space Z height in cm"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("smooth_area"),
			TEXT("Smooth landscape heights in a rectangular region using a 3x3 box-average filter.\n"
			     "Params: x1,y1,x2,y2 (int quad coords), iterations (int, default 1, max 16).\n"
			     "Warning: Rect must be at least 3x3."))
		.RequiredInteger(TEXT("x1"),         TEXT("Min X in quad coords"))
		.RequiredInteger(TEXT("y1"),         TEXT("Min Y in quad coords"))
		.RequiredInteger(TEXT("x2"),         TEXT("Max X in quad coords"))
		.RequiredInteger(TEXT("y2"),         TEXT("Max Y in quad coords"))
		.OptionalInteger(TEXT("iterations"), TEXT("Number of smoothing passes (default 1)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("paint_layer"),
			TEXT("Paint a weightmap layer uniformly across a rectangular region.\n"
			     "Params: x1,y1,x2,y2 (int quad coords), layer_name (string, landscape layer name), alpha (number 0-1, default 1).\n"
			     "Workflow: Call landscape/list_layers to discover valid layer_name values."))
		.RequiredInteger(TEXT("x1"),         TEXT("Min X in quad coords"))
		.RequiredInteger(TEXT("y1"),         TEXT("Min Y in quad coords"))
		.RequiredInteger(TEXT("x2"),         TEXT("Max X in quad coords"))
		.RequiredInteger(TEXT("y2"),         TEXT("Max Y in quad coords"))
		.RequiredString (TEXT("layer_name"), TEXT("Landscape layer name"))
		.OptionalNumber (TEXT("alpha"),      TEXT("Layer alpha 0-1 (default 1)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("list_layers"),
			TEXT("Enumerate landscape weightmap layers (and their LayerInfo assets) on the current ALandscape.\n"
			     "Params: (none).\n"
			     "Returns: {layers: [{layer_name, asset_path?, has_info}]}.\n"
			     "Workflow: call before paint_layer to confirm a valid layer_name."))
		.Build());

	return Tools;
}

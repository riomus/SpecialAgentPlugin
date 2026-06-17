// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PostProcessService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
	static APostProcessVolume* FindPPVByLabel(UWorld* World, const FString& ActorLabel)
	{
		AActor* A = FMCPActorResolver::ByLabel(World, ActorLabel);
		return Cast<APostProcessVolume>(A);
	}

	// Read a JSON array into FVector4: accepts [X,Y,Z] (with W=1) or [X,Y,Z,W].
	static bool ReadVec4(const TSharedPtr<FJsonObject>& Params, const FString& Field, FVector4& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(Field, Arr)) return false;
		if (Arr->Num() < 3) return false;
		Out.X = (*Arr)[0]->AsNumber();
		Out.Y = (*Arr)[1]->AsNumber();
		Out.Z = (*Arr)[2]->AsNumber();
		Out.W = (Arr->Num() >= 4) ? (*Arr)[3]->AsNumber() : 1.0;
		return true;
	}
}

FString FPostProcessService::GetServiceDescription() const
{
	return TEXT("Post-process volumes - spawn and tune exposure, bloom, DOF, color grading, indirect GI");
}

FMCPResponse FPostProcessService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("spawn_volume")) return HandleSpawnVolume(Request);
	if (MethodName == TEXT("set_exposure")) return HandleSetExposure(Request);
	if (MethodName == TEXT("set_bloom")) return HandleSetBloom(Request);
	if (MethodName == TEXT("set_dof")) return HandleSetDof(Request);
	if (MethodName == TEXT("set_color_grading")) return HandleSetColorGrading(Request);
	if (MethodName == TEXT("set_gi")) return HandleSetGi(Request);

	return MethodNotFound(Request.Id, TEXT("post_process"), MethodName);
}

FMCPResponse FPostProcessService::HandleSpawnVolume(const FMCPRequest& Request)
{
	FVector Location(0, 0, 0);
	bool bUnbound = true;
	FString ActorLabel;

	if (Request.Params.IsValid())
	{
		FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);
		FMCPJson::ReadBool(Request.Params, TEXT("unbound"), bUnbound);
		FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel);
	}

	auto Task = [Location, bUnbound, ActorLabel]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn post-process volume")));

		FActorSpawnParameters SpawnParams;
		APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
		if (!Volume) return FMCPJson::MakeError(TEXT("Failed to spawn APostProcessVolume"));

		Volume->bUnbound = bUnbound;
		if (!ActorLabel.IsEmpty())
		{
			Volume->SetActorLabel(ActorLabel);
		}
		Volume->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		FMCPJson::WriteActor(ActorJson, Volume);
		ActorJson->SetBoolField(TEXT("unbound"), Volume->bUnbound);
		ActorJson->SetNumberField(TEXT("priority"), Volume->Priority);
		Result->SetObjectField(TEXT("actor"), ActorJson);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: post_process/spawn_volume -> %s (unbound=%s)"),
			*Volume->GetActorLabel(), bUnbound ? TEXT("true") : TEXT("false"));
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPostProcessService::HandleSetExposure(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel;
	double Bias = 0.0;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("exposure_bias"), Bias))
		return InvalidParams(Request.Id, TEXT("Missing 'exposure_bias' (number)"));

	auto Task = [ActorLabel, Bias]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		APostProcessVolume* V = FindPPVByLabel(World, ActorLabel);
		if (!V) return FMCPJson::MakeError(FString::Printf(TEXT("PostProcessVolume not found: %s"), *ActorLabel));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set exposure")));
		V->Modify();
		V->Settings.bOverride_AutoExposureBias = true;
		V->Settings.AutoExposureBias = static_cast<float>(Bias);
		V->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_label"), ActorLabel);
		Result->SetNumberField(TEXT("exposure_bias"), Bias);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: post_process/set_exposure %s -> %f"), *ActorLabel, Bias);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPostProcessService::HandleSetBloom(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel;
	double Intensity = 0.0;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("intensity"), Intensity))
		return InvalidParams(Request.Id, TEXT("Missing 'intensity' (number)"));

	auto Task = [ActorLabel, Intensity]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		APostProcessVolume* V = FindPPVByLabel(World, ActorLabel);
		if (!V) return FMCPJson::MakeError(FString::Printf(TEXT("PostProcessVolume not found: %s"), *ActorLabel));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set bloom")));
		V->Modify();
		V->Settings.bOverride_BloomIntensity = true;
		V->Settings.BloomIntensity = static_cast<float>(Intensity);
		V->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_label"), ActorLabel);
		Result->SetNumberField(TEXT("intensity"), Intensity);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: post_process/set_bloom %s -> %f"), *ActorLabel, Intensity);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPostProcessService::HandleSetDof(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));

	double FocalDistance = 0.0, Fstop = 0.0;
	bool bHasFocal = FMCPJson::ReadNumber(Request.Params, TEXT("focal_distance"), FocalDistance);
	bool bHasFstop = FMCPJson::ReadNumber(Request.Params, TEXT("f_stop"), Fstop);

	if (!bHasFocal && !bHasFstop)
		return InvalidParams(Request.Id, TEXT("Provide at least one of 'focal_distance' or 'f_stop'"));

	auto Task = [ActorLabel, bHasFocal, FocalDistance, bHasFstop, Fstop]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		APostProcessVolume* V = FindPPVByLabel(World, ActorLabel);
		if (!V) return FMCPJson::MakeError(FString::Printf(TEXT("PostProcessVolume not found: %s"), *ActorLabel));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set depth of field")));
		V->Modify();
		if (bHasFocal)
		{
			V->Settings.bOverride_DepthOfFieldFocalDistance = true;
			V->Settings.DepthOfFieldFocalDistance = static_cast<float>(FocalDistance);
		}
		if (bHasFstop)
		{
			V->Settings.bOverride_DepthOfFieldFstop = true;
			V->Settings.DepthOfFieldFstop = static_cast<float>(Fstop);
		}
		V->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_label"), ActorLabel);
		if (bHasFocal) Result->SetNumberField(TEXT("focal_distance"), FocalDistance);
		if (bHasFstop) Result->SetNumberField(TEXT("f_stop"), Fstop);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: post_process/set_dof %s"), *ActorLabel);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPostProcessService::HandleSetColorGrading(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));

	FVector4 Saturation, Contrast;
	bool bHasSat = ReadVec4(Request.Params, TEXT("saturation"), Saturation);
	bool bHasContrast = ReadVec4(Request.Params, TEXT("contrast"), Contrast);
	if (!bHasSat && !bHasContrast)
		return InvalidParams(Request.Id, TEXT("Provide at least one of 'saturation' or 'contrast' (array [R,G,B] or [R,G,B,A])"));

	auto Task = [ActorLabel, bHasSat, Saturation, bHasContrast, Contrast]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		APostProcessVolume* V = FindPPVByLabel(World, ActorLabel);
		if (!V) return FMCPJson::MakeError(FString::Printf(TEXT("PostProcessVolume not found: %s"), *ActorLabel));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set color grading")));
		V->Modify();
		if (bHasSat)
		{
			V->Settings.bOverride_ColorSaturation = true;
			V->Settings.ColorSaturation = Saturation;
		}
		if (bHasContrast)
		{
			V->Settings.bOverride_ColorContrast = true;
			V->Settings.ColorContrast = Contrast;
		}
		V->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_label"), ActorLabel);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: post_process/set_color_grading %s"), *ActorLabel);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FPostProcessService::HandleSetGi(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel;
	double Intensity = 0.0;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("intensity"), Intensity))
		return InvalidParams(Request.Id, TEXT("Missing 'intensity' (number)"));

	auto Task = [ActorLabel, Intensity]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		APostProcessVolume* V = FindPPVByLabel(World, ActorLabel);
		if (!V) return FMCPJson::MakeError(FString::Printf(TEXT("PostProcessVolume not found: %s"), *ActorLabel));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set global illumination")));
		V->Modify();
		V->Settings.bOverride_IndirectLightingIntensity = true;
		V->Settings.IndirectLightingIntensity = static_cast<float>(Intensity);
		V->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_label"), ActorLabel);
		Result->SetNumberField(TEXT("intensity"), Intensity);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: post_process/set_gi %s -> %f"), *ActorLabel, Intensity);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

TArray<FMCPToolInfo> FPostProcessService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(TEXT("spawn_volume"),
		TEXT("Spawn an APostProcessVolume into the editor world (Unbound by default = applies to the whole level, ignoring its bounds). "
			 "Returns {actor:{actor_label, ..., unbound, priority}}; use actor.actor_label as the actor_label for every set_* tool below. "
			 "Params: location (array [X,Y,Z] world-space cm, optional, default origin [0,0,0]), unbound (boolean, optional, default true), actor_label (string, optional - auto-generated label if omitted). "
			 "Workflow: spawn_volume -> set_exposure / set_bloom / set_dof / set_color_grading / set_gi referencing the returned actor_label. "
			 "Warning: one volume of a given purpose is usually enough - overlapping volumes blend by Priority, so give labels and watch for duplicates; bounded (unbound=false) volumes only affect cameras inside their box. Not saved to disk - save the level to persist."))
		.OptionalVec3(TEXT("location"), TEXT("World-space [X,Y,Z] in cm; default origin [0,0,0]"))
		.OptionalBool(TEXT("unbound"), TEXT("If true the volume affects the entire world regardless of bounds; default true"))
		.OptionalString(TEXT("actor_label"), TEXT("Custom actor label used as the handle for subsequent set_* calls; auto-generated if omitted"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_exposure"),
		TEXT("Override AutoExposureBias (overall exposure compensation, in EV stops) on an existing PostProcessVolume. "
			 "Returns {actor_label, exposure_bias}. "
			 "Params: actor_label (string, label of a spawned PostProcessVolume, required), exposure_bias (number, EV stops, required, typical -15..15; +1 = one stop brighter). "
			 "Workflow: spawn_volume first, then set_exposure -> screenshot/capture to verify scene brightness. "
			 "Warning: also sets bOverride_AutoExposureBias=true so the value takes effect; this is a bias on top of auto-exposure metering, not an absolute level; volume not found by label errors; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned PostProcessVolume"))
		.RequiredNumber(TEXT("exposure_bias"), TEXT("Exposure compensation in EV stops; typical -15..15"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_bloom"),
		TEXT("Override BloomIntensity (glow from bright pixels) on an existing PostProcessVolume. "
			 "Returns {actor_label, intensity}. "
			 "Params: actor_label (string, PostProcessVolume label, required), intensity (number, unitless multiplier, required, typical 0..8; 0 disables bloom). "
			 "Workflow: spawn_volume -> set_bloom; pair with set_exposure to balance the overall look. "
			 "Warning: also sets bOverride_BloomIntensity=true; very high intensity washes out the image; volume not found by label errors; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned PostProcessVolume"))
		.RequiredNumber(TEXT("intensity"), TEXT("Bloom intensity multiplier (unitless); typical 0..8, 0 disables"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_dof"),
		TEXT("Override cinematic depth-of-field: DepthOfFieldFocalDistance (cm) and/or DepthOfFieldFstop (aperture) on a PostProcessVolume. "
			 "Returns {actor_label, focal_distance?, f_stop?} echoing only the fields you set. "
			 "Params: actor_label (string, required), focal_distance (number, cm = distance from camera to the in-focus plane, optional), f_stop (number, aperture ~1..32, optional; lower f_stop = shallower focus / more blur). "
			 "Workflow: trace the focus target to get a distance (e.g. viewport/trace_from_screen) -> set focal_distance to it; tune f_stop for blur amount. "
			 "Warning: at least one of focal_distance / f_stop is required or the call errors; each provided value also sets its bOverride_ flag; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned PostProcessVolume"))
		.OptionalNumber(TEXT("focal_distance"), TEXT("Distance from camera to the in-focus plane, in cm"))
		.OptionalNumber(TEXT("f_stop"), TEXT("Aperture f-number, typically 1..32; lower = shallower depth of field"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_color_grading"),
		TEXT("Override the GLOBAL ColorSaturation and/or ColorContrast color-grading wheels on a PostProcessVolume (per-channel RGBY multipliers). "
			 "Returns {actor_label}. Each value is a 4-component vector [R,G,B,Y] where neutral (no change) is [1,1,1,1]; the 4th component is the master/luminance term, not alpha. "
			 "Params: actor_label (string, required), saturation (array [R,G,B] or [R,G,B,A], optional), contrast (same shape, optional). "
			 "Workflow: spawn_volume -> set_color_grading; provide at least one of saturation/contrast; call again to fine-tune. "
			 "Warning: at least one of saturation/contrast is required; the 4th component defaults to 1.0 when you pass only [R,G,B]; this sets the GLOBAL wheels only - per-region shadows/midtones/highlights wheels are not exposed here; each provided value sets its bOverride_ flag; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned PostProcessVolume"))
		.OptionalColor(TEXT("saturation"), TEXT("Global ColorSaturation [R,G,B] or [R,G,B,Y]; neutral [1,1,1,1]"))
		.OptionalColor(TEXT("contrast"), TEXT("Global ColorContrast [R,G,B] or [R,G,B,Y]; neutral [1,1,1,1]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_gi"),
		TEXT("Override IndirectLightingIntensity (a multiplier on bounced / indirect global-illumination light) on a PostProcessVolume. "
			 "Returns {actor_label, intensity}. "
			 "Params: actor_label (string, required), intensity (number, unitless multiplier, required, typical 0..4; 1.0 = unchanged, 0 = no indirect light). "
			 "Workflow: spawn_volume -> set_gi to dial the overall bounce-light level. "
			 "Warning: also sets bOverride_IndirectLightingIntensity=true; in UE5.7 Lumen (the default dynamic GI) honors this live with no lighting build needed, and baked static lighting honors it too; volume not found errors; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned PostProcessVolume"))
		.RequiredNumber(TEXT("intensity"), TEXT("Indirect/bounce lighting multiplier (unitless); typical 0..4, 1.0 = unchanged"))
		.Build());

	return Tools;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PostProcessService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"

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

FMCPResponse FPostProcessService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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

		FActorSpawnParameters SpawnParams;
		APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
		if (!Volume) return FMCPJson::MakeError(TEXT("Failed to spawn APostProcessVolume"));

		Volume->bUnbound = bUnbound;
		if (!ActorLabel.IsEmpty())
		{
			Volume->SetActorLabel(ActorLabel);
		}

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
		TEXT("Spawn APostProcessVolume at a location (default Unbound for whole-level effect).\n"
			 "Params: location ([X,Y,Z] cm, optional), unbound (bool, default true), actor_label (string, optional).\n"
			 "Workflow: spawn first, then call set_exposure/set_bloom/set_dof/set_color_grading/set_gi referencing its label.\n"
			 "Warning: overlapping unbound volumes blend by priority - name them for clarity."))
		.OptionalVec3(TEXT("location"), TEXT("World-space [X,Y,Z] cm (defaults to origin)"))
		.OptionalBool(TEXT("unbound"), TEXT("If true, volume affects the entire world (default true)"))
		.OptionalString(TEXT("actor_label"), TEXT("Custom actor label (identifier for subsequent set_* calls)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_exposure"),
		TEXT("Override AutoExposureBias on a PostProcessVolume (EV units).\n"
			 "Params: actor_label (string), exposure_bias (number, EV, typical -15..15).\n"
			 "Workflow: use screenshot/capture after adjusting to verify scene brightness.\n"
			 "Warning: enables bOverride_AutoExposureBias; compounds with per-camera exposure."))
		.RequiredString(TEXT("actor_label"), TEXT("PostProcessVolume actor label"))
		.RequiredNumber(TEXT("exposure_bias"), TEXT("EV bias; typical -15..15"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_bloom"),
		TEXT("Override BloomIntensity on a PostProcessVolume.\n"
			 "Params: actor_label (string), intensity (number, 0..8 typical).\n"
			 "Workflow: pair with set_exposure to balance the overall look.\n"
			 "Warning: very high intensity may wash out the image."))
		.RequiredString(TEXT("actor_label"), TEXT("PostProcessVolume actor label"))
		.RequiredNumber(TEXT("intensity"), TEXT("Bloom intensity; typical 0..8"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_dof"),
		TEXT("Override DepthOfFieldFocalDistance and/or DepthOfFieldFstop on a PostProcessVolume.\n"
			 "Params: actor_label (string), focal_distance (number, cm, optional), f_stop (number, 1..32, optional).\n"
			 "Workflow: query focus target via viewport/trace_from_screen to compute focal_distance.\n"
			 "Warning: at least one of focal_distance / f_stop is required."))
		.RequiredString(TEXT("actor_label"), TEXT("PostProcessVolume actor label"))
		.OptionalNumber(TEXT("focal_distance"), TEXT("Focal distance in cm"))
		.OptionalNumber(TEXT("f_stop"), TEXT("Aperture F-stop (1..32)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_color_grading"),
		TEXT("Override global ColorSaturation and/or ColorContrast on a PostProcessVolume.\n"
			 "Params: actor_label (string), saturation (array [R,G,B] or [R,G,B,A]), contrast (same shape).\n"
			 "Workflow: provide at least one; call again for fine-tuning.\n"
			 "Warning: alpha defaults to 1.0. Per-tone-region sliders (shadows/midtones/highlights) not exposed here."))
		.RequiredString(TEXT("actor_label"), TEXT("PostProcessVolume actor label"))
		.OptionalColor(TEXT("saturation"), TEXT("[R,G,B] or [R,G,B,A] for ColorSaturation"))
		.OptionalColor(TEXT("contrast"), TEXT("[R,G,B] or [R,G,B,A] for ColorContrast"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_gi"),
		TEXT("Override IndirectLightingIntensity on a PostProcessVolume.\n"
			 "Params: actor_label (string), intensity (number, 0..4 typical).\n"
			 "Workflow: adjust after building lighting to dial bounce-light level.\n"
			 "Warning: Lumen and static lighting both honor this multiplier."))
		.RequiredString(TEXT("actor_label"), TEXT("PostProcessVolume actor label"))
		.RequiredNumber(TEXT("intensity"), TEXT("Indirect lighting intensity; typical 0..4"))
		.Build());

	return Tools;
}

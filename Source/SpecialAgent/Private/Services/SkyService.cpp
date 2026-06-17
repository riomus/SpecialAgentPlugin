// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/SkyService.h"
#include "MCPCommon/MCPRequestContext.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/DirectionalLight.h"
#include "ScopedTransaction.h"

namespace
{
	// Spawn a simple AInfo-derived actor at the requested location/label.
	template<typename TActor>
	static TSharedPtr<FJsonObject> SpawnSimple(const FVector& Location, const FString& ActorLabel, const TCHAR* Verb)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("SpecialAgent: %s"), Verb)));

		FActorSpawnParameters SP;
		TActor* Actor = World->SpawnActor<TActor>(TActor::StaticClass(), Location, FRotator::ZeroRotator, SP);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Failed to spawn %s"), Verb));

		if (!ActorLabel.IsEmpty())
		{
			Actor->SetActorLabel(ActorLabel);
		}
		Actor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		FMCPJson::WriteActor(ActorJson, Actor);
		Result->SetObjectField(TEXT("actor"), ActorJson);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: sky/%s -> %s"), Verb, *Actor->GetActorLabel());
		return Result;
	}

}

FString FSkyService::GetServiceDescription() const
{
	return TEXT("Sky & atmosphere - spawn SkyAtmosphere/HeightFog/VolumetricCloud/SkyLight; set sun angle");
}

FMCPResponse FSkyService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("spawn_sky_atmosphere")) return HandleSpawnSkyAtmosphere(Request);
	if (MethodName == TEXT("spawn_height_fog")) return HandleSpawnHeightFog(Request);
	if (MethodName == TEXT("spawn_cloud")) return HandleSpawnCloud(Request);
	if (MethodName == TEXT("spawn_sky_light")) return HandleSpawnSkyLight(Request);
	if (MethodName == TEXT("set_sun_angle")) return HandleSetSunAngle(Request);

	return MethodNotFound(Request.Id, TEXT("sky"), MethodName);
}

namespace
{
	template<typename TActor>
	static FMCPResponse DispatchSpawnActor(const FMCPRequest& Request, const TCHAR* Verb)
	{
		FVector Location(0, 0, 0);
		FString ActorLabel;
		if (Request.Params.IsValid())
		{
			FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);
			FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel);
		}
		auto Task = [Location, ActorLabel, Verb]() -> TSharedPtr<FJsonObject>
		{
			return SpawnSimple<TActor>(Location, ActorLabel, Verb);
		};
		return FMCPResponse::Success(Request.Id,
			FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
	}
}

FMCPResponse FSkyService::HandleSpawnSkyAtmosphere(const FMCPRequest& Request)
{
	return DispatchSpawnActor<ASkyAtmosphere>(Request, TEXT("spawn_sky_atmosphere"));
}

FMCPResponse FSkyService::HandleSpawnHeightFog(const FMCPRequest& Request)
{
	return DispatchSpawnActor<AExponentialHeightFog>(Request, TEXT("spawn_height_fog"));
}

FMCPResponse FSkyService::HandleSpawnCloud(const FMCPRequest& Request)
{
	return DispatchSpawnActor<AVolumetricCloud>(Request, TEXT("spawn_cloud"));
}

FMCPResponse FSkyService::HandleSpawnSkyLight(const FMCPRequest& Request)
{
	return DispatchSpawnActor<ASkyLight>(Request, TEXT("spawn_sky_light"));
}

FMCPResponse FSkyService::HandleSetSunAngle(const FMCPRequest& Request)
{
	FString ActorLabel;
	if (Request.Params.IsValid())
	{
		FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel);
	}

	double Pitch = 0.0, Yaw = 0.0, TimeOfDay = 0.0;
	bool bHasPitch = Request.Params.IsValid() && FMCPJson::ReadNumber(Request.Params, TEXT("pitch"), Pitch);
	bool bHasYaw = Request.Params.IsValid() && FMCPJson::ReadNumber(Request.Params, TEXT("yaw"), Yaw);
	bool bHasToD = Request.Params.IsValid() && FMCPJson::ReadNumber(Request.Params, TEXT("time_of_day"), TimeOfDay);

	if (!bHasToD && !(bHasPitch || bHasYaw))
	{
		return InvalidParams(Request.Id, TEXT("Provide 'time_of_day' (0-24 hours) OR 'pitch'/'yaw' degrees"));
	}

	auto Task = [ActorLabel, bHasPitch, Pitch, bHasYaw, Yaw, bHasToD, TimeOfDay]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		ADirectionalLight* Sun = nullptr;
		if (!ActorLabel.IsEmpty())
		{
			AActor* Any = FMCPActorResolver::ByLabel(World, ActorLabel);
			Sun = Cast<ADirectionalLight>(Any);
			if (!Sun) return FMCPJson::MakeError(FString::Printf(TEXT("Actor '%s' is not an ADirectionalLight"), *ActorLabel));
		}
		else
		{
			for (TActorIterator<ADirectionalLight> It(World); It; ++It) { Sun = *It; break; }
			if (!Sun) return FMCPJson::MakeError(TEXT("No ADirectionalLight in level. Provide 'actor_label' or spawn one first."));
		}

		FRotator Current = Sun->GetActorRotation();
		float NewPitch = Current.Pitch;
		float NewYaw = Current.Yaw;

		if (bHasToD)
		{
			// Map 0..24 hours to pitch: 6=horizon east (0), 12=overhead (-90), 18=horizon west (-180), 0=below (+90).
			const double Hours = FMath::Fmod(FMath::Max(0.0, TimeOfDay), 24.0);
			NewPitch = static_cast<float>(-(Hours / 24.0 * 360.0 - 90.0));
		}
		if (bHasPitch) NewPitch = static_cast<float>(Pitch);
		if (bHasYaw) NewYaw = static_cast<float>(Yaw);

		const FRotator NewRot(NewPitch, NewYaw, Current.Roll);
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set sun angle")));
		Sun->Modify();
		Sun->SetActorRotation(NewRot);
		Sun->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		FMCPJson::WriteActor(ActorJson, Sun);
		Result->SetObjectField(TEXT("actor"), ActorJson);
		FMCPJson::WriteRotator(Result, TEXT("rotation"), NewRot);
		if (bHasToD) Result->SetNumberField(TEXT("time_of_day"), TimeOfDay);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: sky/set_sun_angle %s pitch=%f yaw=%f"),
			*Sun->GetActorLabel(), NewRot.Pitch, NewRot.Yaw);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

TArray<FMCPToolInfo> FSkyService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	auto BuildSpawnTool = [](const TCHAR* Name, const TCHAR* Description)
	{
		return FMCPToolBuilder(Name, Description)
			.OptionalVec3(TEXT("location"), TEXT("World-space spawn location [X,Y,Z] in cm, default origin [0,0,0]"))
			.OptionalString(TEXT("actor_label"), TEXT("Custom editor label for the spawned actor (optional)"))
			.Build();
	};

	Tools.Add(BuildSpawnTool(TEXT("spawn_sky_atmosphere"),
		TEXT("Spawn an ASkyAtmosphere actor: physically-based sky color, horizon, and aerial perspective. "
			 "Returns {success, actor:{actor_label, ...}}; pass the returned actor_label to follow-up tools. "
			 "Params: location (world-space cm [X,Y,Z], optional, default origin; position is irrelevant since the effect is global), actor_label (string, optional). "
			 "Workflow: spawn a directional light (lighting/spawn_light) as the atmosphere sun and an ASkyLight (sky/spawn_sky_light) for indirect capture; rotate the sun via sky/set_sun_angle. "
			 "Warning: spawns into the in-memory level only (not saved). One atmosphere per level is enough; extra ones double-apply scattering. Its internal distances are in KILOMETERS (ground_radius ~6360, atmosphere_height ~60), not cm.")));

	Tools.Add(BuildSpawnTool(TEXT("spawn_height_fog"),
		TEXT("Spawn an AExponentialHeightFog actor: exponential atmospheric fog with height-based density falloff. "
			 "Returns {success, actor:{actor_label, ...}}; pass the returned actor_label to follow-up tools. "
			 "Params: location (world-space cm [X,Y,Z], optional, default origin), actor_label (string, optional). "
			 "Workflow: pair with sky/spawn_sky_atmosphere; tune density/color/volumetric-fog through the post-process or fog tools. "
			 "Warning: spawns into the in-memory level only (not saved). Only one height-fog actor is sampled by the engine, so reuse an existing one rather than stacking duplicates. Fog distances are in cm, density/falloff are unitless.")));

	Tools.Add(BuildSpawnTool(TEXT("spawn_cloud"),
		TEXT("Spawn an AVolumetricCloud actor: a volumetric cloud layer rendered above an ASkyAtmosphere. "
			 "Returns {success, actor:{actor_label, ...}}; pass the returned actor_label to follow-up tools. "
			 "Params: location (world-space cm [X,Y,Z], optional, default origin; the layer is global so position is irrelevant), actor_label (string, optional). "
			 "Workflow: requires an ASkyAtmosphere (sky/spawn_sky_atmosphere) plus an atmosphere-sun directional light for correct scattering; a Volume-domain cloud material gives best results. "
			 "Warning: spawns into the in-memory level only (not saved). GPU-heavy, so avoid on low-spec previews. Cloud altitudes/thickness are in KILOMETERS, not cm.")));

	Tools.Add(BuildSpawnTool(TEXT("spawn_sky_light"),
		TEXT("Spawn an ASkyLight actor: captures the surrounding sky/scene into a cubemap for ambient and image-based (IBL) indirect lighting. "
			 "Returns {success, actor:{actor_label, ...}}; pass the returned actor_label to the lighting/set_* tools. "
			 "Params: location (world-space cm [X,Y,Z], optional, default origin; position is irrelevant for the global capture), actor_label (string, optional). "
			 "Workflow: spawn after sky/spawn_sky_atmosphere and the sun; the SkyLight captures the scene once on spawn/load, so re-capture (Recapture Scene in the editor, or enable Real Time Capture) after later sky changes. "
			 "Warning: spawns into the in-memory level only (not saved). Only one SkyLight is sampled per scene; reuse an existing one. SkyLight intensity is a unitless scale (default 1).")));

	Tools.Add(FMCPToolBuilder(TEXT("set_sun_angle"),
		TEXT("Rotate a directional light (the sun) to an explicit pitch/yaw or a time-of-day, driving the sky lighting. "
			 "Returns {success, actor:{actor_label, ...}, rotation:[pitch,yaw,roll]} plus time_of_day when that was supplied. "
			 "Params: actor_label (string, optional; targets that ADirectionalLight, else uses the first directional light in the level), "
			 "time_of_day (number, 0-24 hours, optional; mapped to pitch as 0=midnight/below, 6=sunrise on horizon, 12=noon overhead, 18=sunset), "
			 "pitch (number, degrees, optional; overrides time_of_day's pitch when both are given), "
			 "yaw (number, degrees, optional; compass heading, applied independently). "
			 "Provide time_of_day OR pitch/yaw (at least one is required). "
			 "Workflow: spawn the sun via lighting/spawn_light and an ASkyAtmosphere via sky/spawn_sky_atmosphere first; for the sun to drive the atmosphere it must be the Atmosphere Sun Light; recapture the SkyLight and adjust exposure after large changes. "
			 "Warning: edits the in-memory actor rotation and marks the package dirty, but does not save the level. time_of_day sets pitch only, so set yaw separately for heading. Note the Details panel labels rotation as X=Roll/Y=Pitch/Z=Yaw, which does not match this [pitch,yaw,roll] order. Errors if actor_label is not a directional light, or if no directional light exists."))
		.OptionalString(TEXT("actor_label"), TEXT("Editor label of the ADirectionalLight (optional; defaults to the first one found)"))
		.OptionalNumber(TEXT("time_of_day"), TEXT("Hours 0-24 (0=midnight, 6=sunrise, 12=noon, 18=sunset); sets pitch only"))
		.OptionalNumber(TEXT("pitch"), TEXT("Explicit pitch in degrees (overrides the time_of_day-derived pitch)"))
		.OptionalNumber(TEXT("yaw"), TEXT("Yaw in degrees (compass heading), applied independently"))
		.Build());

	return Tools;
}

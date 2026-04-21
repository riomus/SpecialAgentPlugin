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

namespace
{
	// Spawn a simple AInfo-derived actor at the requested location/label.
	template<typename TActor>
	static TSharedPtr<FJsonObject> SpawnSimple(const FVector& Location, const FString& ActorLabel, const TCHAR* Verb)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		FActorSpawnParameters SP;
		TActor* Actor = World->SpawnActor<TActor>(TActor::StaticClass(), Location, FRotator::ZeroRotator, SP);
		if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Failed to spawn %s"), Verb));

		if (!ActorLabel.IsEmpty())
		{
			Actor->SetActorLabel(ActorLabel);
		}

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
			.OptionalVec3(TEXT("location"), TEXT("World-space [X,Y,Z] cm (defaults to origin)"))
			.OptionalString(TEXT("actor_label"), TEXT("Optional custom actor label"))
			.Build();
	};

	Tools.Add(BuildSpawnTool(TEXT("spawn_sky_atmosphere"),
		TEXT("Spawn ASkyAtmosphere. Provides physically-based sky color and aerial perspective.\n"
			 "Params: location ([X,Y,Z] optional), actor_label (string optional).\n"
			 "Workflow: pair with a DirectionalLight for the sun and ASkyLight for indirect sky capture.\n"
			 "Warning: one atmosphere per level is typical; multiples can produce undefined results.")));

	Tools.Add(BuildSpawnTool(TEXT("spawn_height_fog"),
		TEXT("Spawn AExponentialHeightFog - exponential fog with height falloff.\n"
			 "Params: location ([X,Y,Z] optional), actor_label (string optional).\n"
			 "Workflow: edit Settings on the spawned actor (future tools) for density/color.\n"
			 "Warning: only one fog actor is rendered at a time by the engine.")));

	Tools.Add(BuildSpawnTool(TEXT("spawn_cloud"),
		TEXT("Spawn AVolumetricCloud - volumetric clouds atop an ASkyAtmosphere.\n"
			 "Params: location ([X,Y,Z] optional), actor_label (string optional).\n"
			 "Workflow: requires an ASkyAtmosphere for correct scattering.\n"
			 "Warning: volumetric clouds are GPU heavy; disable on low-spec previews.")));

	Tools.Add(BuildSpawnTool(TEXT("spawn_sky_light"),
		TEXT("Spawn ASkyLight. Captures the sky environment for indirect/IBL lighting.\n"
			 "Params: location ([X,Y,Z] optional), actor_label (string optional).\n"
			 "Workflow: trigger 'Recapture Scene' via editor or rebuild lighting for accurate capture.\n"
			 "Warning: only one active SkyLight is sampled per scene.")));

	Tools.Add(FMCPToolBuilder(TEXT("set_sun_angle"),
		TEXT("Rotate an ADirectionalLight (the sun) to a given angle or time-of-day.\n"
			 "Params: actor_label (string optional, defaults to the first ADirectionalLight found), "
			 "time_of_day (number, 0-24 hours; maps to pitch), pitch (number, degrees), yaw (number, degrees).\n"
			 "Workflow: pair with post_process/set_exposure to respect the lighting change.\n"
			 "Warning: time_of_day drives pitch only; set yaw separately for compass heading."))
		.OptionalString(TEXT("actor_label"), TEXT("ADirectionalLight actor label (optional)"))
		.OptionalNumber(TEXT("time_of_day"), TEXT("Hours 0-24 (0=midnight, 6=sunrise, 12=noon, 18=sunset); maps to pitch"))
		.OptionalNumber(TEXT("pitch"), TEXT("Explicit pitch in degrees (overrides time_of_day)"))
		.OptionalNumber(TEXT("yaw"), TEXT("Yaw in degrees (compass direction)"))
		.Build());

	return Tools;
}

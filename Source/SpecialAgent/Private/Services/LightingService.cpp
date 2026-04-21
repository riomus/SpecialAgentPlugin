// Copyright Epic Games, Inc. All Rights Reserved.
// LightingService: direct C++ implementation for 6 lighting tools.

#include "Services/LightingService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "Editor.h"
#include "Engine/World.h"

#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Engine/Light.h"

#include "Components/LightComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/LocalLightComponent.h"

#include "EditorBuildUtils.h"

FLightingService::FLightingService()
{
}

FString FLightingService::GetServiceDescription() const
{
	return TEXT("Lighting control - spawn lights, configure, and build lightmaps");
}

FMCPResponse FLightingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("spawn_light")) return HandleSpawnLight(Request);
	if (MethodName == TEXT("set_light_intensity")) return HandleSetLightIntensity(Request);
	if (MethodName == TEXT("set_light_color")) return HandleSetLightColor(Request);
	if (MethodName == TEXT("set_light_attenuation")) return HandleSetLightAttenuation(Request);
	if (MethodName == TEXT("set_light_cast_shadows")) return HandleSetLightCastShadows(Request);
	if (MethodName == TEXT("build_lighting")) return HandleBuildLighting(Request);

	return MethodNotFound(Request.Id, TEXT("lighting"), MethodName);
}

// -----------------------------------------------------------------------------
// spawn_light
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleSpawnLight(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString LightType;
	if (!FMCPJson::ReadString(Request.Params, TEXT("light_type"), LightType))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'light_type' (point|spot|directional|rect|sky)"));
	}
	LightType = LightType.ToLower();

	FVector Location(0, 0, 0);
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' [X, Y, Z]"));
	}

	FRotator Rotation(0, 0, 0);
	const bool bHasRotation = FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

	double Intensity = -1.0;
	const bool bHasIntensity = FMCPJson::ReadNumber(Request.Params, TEXT("intensity"), Intensity);

	FLinearColor Color(1, 1, 1, 1);
	const bool bHasColor = FMCPJson::ReadColor(Request.Params, TEXT("color"), Color);

	auto Task = [LightType, Location, Rotation, bHasRotation, Intensity, bHasIntensity, Color, bHasColor]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		UClass* SpawnClass = nullptr;
		if      (LightType == TEXT("point"))       SpawnClass = APointLight::StaticClass();
		else if (LightType == TEXT("spot"))        SpawnClass = ASpotLight::StaticClass();
		else if (LightType == TEXT("directional")) SpawnClass = ADirectionalLight::StaticClass();
		else if (LightType == TEXT("rect"))        SpawnClass = ARectLight::StaticClass();
		else if (LightType == TEXT("sky"))         SpawnClass = ASkyLight::StaticClass();
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: spawn_light unknown light_type '%s'"), *LightType);
			return FMCPJson::MakeError(FString::Printf(TEXT("Unknown light_type: %s"), *LightType));
		}

		FActorSpawnParameters SpawnParams;
		AActor* NewActor = World->SpawnActor<AActor>(SpawnClass, Location, bHasRotation ? Rotation : FRotator::ZeroRotator, SpawnParams);
		if (!NewActor)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: spawn_light SpawnActor returned null for %s"), *LightType);
			return FMCPJson::MakeError(TEXT("Failed to spawn light actor"));
		}

		// Apply intensity / color to the root light component if present.
		if (ALight* LightActor = Cast<ALight>(NewActor))
		{
			if (ULightComponent* LightComp = LightActor->GetLightComponent())
			{
				if (bHasIntensity)
				{
					LightComp->SetIntensity(static_cast<float>(Intensity));
				}
				if (bHasColor)
				{
					LightComp->SetLightColor(Color);
				}
			}
		}
		else if (ASkyLight* SkyLightActor = Cast<ASkyLight>(NewActor))
		{
			if (USkyLightComponent* SkyComp = SkyLightActor->GetLightComponent())
			{
				if (bHasIntensity)
				{
					SkyComp->SetIntensity(static_cast<float>(Intensity));
				}
				if (bHasColor)
				{
					SkyComp->SetLightColor(Color);
				}
			}
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("light_type"), LightType);
		TSharedPtr<FJsonObject> ActorData = MakeShared<FJsonObject>();
		FMCPJson::WriteActor(ActorData, NewActor);
		Result->SetObjectField(TEXT("actor"), ActorData);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Spawned %s light '%s' at (%.1f, %.1f, %.1f)"),
			*LightType, *NewActor->GetActorLabel(), Location.X, Location.Y, Location.Z);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// Small helpers: resolve a light actor by label. Sky lights are not ULightComponent
// subclasses, so we expose two accessors and callers pick the one they need.
// -----------------------------------------------------------------------------
struct FResolvedLight
{
	ULightComponent*     LightComp    = nullptr; // ALight::GetLightComponent (point/spot/directional/rect)
	USkyLightComponent*  SkyLightComp = nullptr; // ASkyLight::GetLightComponent
	ULightComponentBase* BaseComp     = nullptr; // Common base for shadow-cast toggling

	bool IsValid() const { return BaseComp != nullptr; }
};

static FResolvedLight ResolveLight(UWorld* World, const FString& ActorName, FString& ErrorOut)
{
	FResolvedLight R;
	if (!World)
	{
		ErrorOut = TEXT("No editor world");
		return R;
	}
	AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
	if (!Actor)
	{
		ErrorOut = FString::Printf(TEXT("Actor not found: %s"), *ActorName);
		return R;
	}
	if (ALight* LightActor = Cast<ALight>(Actor))
	{
		R.LightComp = LightActor->GetLightComponent();
		R.BaseComp  = R.LightComp;
		return R;
	}
	if (ASkyLight* SkyActor = Cast<ASkyLight>(Actor))
	{
		R.SkyLightComp = SkyActor->GetLightComponent();
		R.BaseComp     = R.SkyLightComp;
		return R;
	}
	ErrorOut = FString::Printf(TEXT("Actor '%s' is not a light"), *ActorName);
	return R;
}

// -----------------------------------------------------------------------------
// set_light_intensity
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleSetLightIntensity(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ActorName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	}
	double Intensity = 0.0;
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("intensity"), Intensity))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'intensity'"));
	}

	const float IntensityF = static_cast<float>(Intensity);

	auto Task = [ActorName, IntensityF]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FString Err;
		FResolvedLight R = ResolveLight(World, ActorName, Err);
		if (!R.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: set_light_intensity failed: %s"), *Err);
			return FMCPJson::MakeError(Err);
		}
		if (R.LightComp)         R.LightComp->SetIntensity(IntensityF);
		else if (R.SkyLightComp) R.SkyLightComp->SetIntensity(IntensityF);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetNumberField(TEXT("intensity"), IntensityF);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: set_light_intensity '%s' = %.2f"), *ActorName, IntensityF);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// set_light_color
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleSetLightColor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ActorName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	}
	FLinearColor Color(1, 1, 1, 1);
	if (!FMCPJson::ReadColor(Request.Params, TEXT("color"), Color))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'color' [R, G, B] (0-1)"));
	}

	auto Task = [ActorName, Color]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FString Err;
		FResolvedLight R = ResolveLight(World, ActorName, Err);
		if (!R.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: set_light_color failed: %s"), *Err);
			return FMCPJson::MakeError(Err);
		}
		if (R.LightComp)         R.LightComp->SetLightColor(Color);
		else if (R.SkyLightComp) R.SkyLightComp->SetLightColor(Color);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		FMCPJson::WriteColor(Result, TEXT("color"), Color);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: set_light_color '%s' = (%.2f, %.2f, %.2f)"),
			*ActorName, Color.R, Color.G, Color.B);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// set_light_attenuation
//   - For point/spot (or any ULocalLightComponent): applies 'radius'.
//   - For spot specifically: also applies 'inner_cone_angle' / 'outer_cone_angle' if provided.
//   - Directional/sky/rect: radius is not applicable; if only radius given, reports error.
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleSetLightAttenuation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ActorName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	}

	double Radius = 0.0;
	const bool bHasRadius = FMCPJson::ReadNumber(Request.Params, TEXT("radius"), Radius);
	double InnerCone = 0.0;
	const bool bHasInner = FMCPJson::ReadNumber(Request.Params, TEXT("inner_cone_angle"), InnerCone);
	double OuterCone = 0.0;
	const bool bHasOuter = FMCPJson::ReadNumber(Request.Params, TEXT("outer_cone_angle"), OuterCone);

	if (!bHasRadius && !bHasInner && !bHasOuter)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one of: 'radius', 'inner_cone_angle', 'outer_cone_angle'"));
	}

	auto Task = [ActorName, bHasRadius, Radius, bHasInner, InnerCone, bHasOuter, OuterCone]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}
		AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
		if (!Actor)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);

		bool bApplied = false;

		// Spot: handle cone angles first, then fall through to local light radius.
		if (ASpotLight* SpotActor = Cast<ASpotLight>(Actor))
		{
			if (USpotLightComponent* Spot = Cast<USpotLightComponent>(SpotActor->GetLightComponent()))
			{
				if (bHasInner)
				{
					Spot->SetInnerConeAngle(static_cast<float>(InnerCone));
					Result->SetNumberField(TEXT("inner_cone_angle"), InnerCone);
					bApplied = true;
				}
				if (bHasOuter)
				{
					Spot->SetOuterConeAngle(static_cast<float>(OuterCone));
					Result->SetNumberField(TEXT("outer_cone_angle"), OuterCone);
					bApplied = true;
				}
			}
		}

		// Point / Spot (UPointLightComponent inherits ULocalLightComponent) / Rect all support attenuation radius.
		if (bHasRadius)
		{
			if (ALight* LightActor = Cast<ALight>(Actor))
			{
				if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(LightActor->GetLightComponent()))
				{
					Local->SetAttenuationRadius(static_cast<float>(Radius));
					Result->SetNumberField(TEXT("radius"), Radius);
					bApplied = true;
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: set_light_attenuation radius not supported on this light type for '%s'"), *ActorName);
					return FMCPJson::MakeError(TEXT("Attenuation radius only applies to local lights (point/spot/rect)"));
				}
			}
			else
			{
				return FMCPJson::MakeError(TEXT("Attenuation radius requires an ALight actor"));
			}
		}

		if (!bApplied)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: set_light_attenuation could not apply any property to '%s'"), *ActorName);
			return FMCPJson::MakeError(TEXT("No attenuation property applied (wrong light type for given fields)"));
		}

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: set_light_attenuation '%s' applied"), *ActorName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// set_light_cast_shadows
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleSetLightCastShadows(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ActorName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
	}
	bool bCast = true;
	if (!FMCPJson::ReadBool(Request.Params, TEXT("cast_shadows"), bCast))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'cast_shadows' (bool)"));
	}

	auto Task = [ActorName, bCast]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FString Err;
		FResolvedLight R = ResolveLight(World, ActorName, Err);
		if (!R.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: set_light_cast_shadows failed: %s"), *Err);
			return FMCPJson::MakeError(Err);
		}
		R.BaseComp->SetCastShadows(bCast);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetBoolField(TEXT("cast_shadows"), bCast);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: set_light_cast_shadows '%s' = %s"), *ActorName, bCast ? TEXT("true") : TEXT("false"));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// build_lighting
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleBuildLighting(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		const bool bOk = FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting, /*bAllowLightingDialog=*/ false);

		TSharedPtr<FJsonObject> Result = bOk ? FMCPJson::MakeSuccess() : FMCPJson::MakeError(TEXT("EditorBuild returned false"));
		Result->SetBoolField(TEXT("build_started"), bOk);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: build_lighting issued (result=%s)"), bOk ? TEXT("true") : TEXT("false"));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// Tool catalog
// -----------------------------------------------------------------------------

TArray<FMCPToolInfo> FLightingService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_light"),
			TEXT("Spawn a point/spot/directional/rect/sky light actor. Returns the new actor label.\n"
			     "Params: light_type (enum point|spot|directional|rect|sky), location ([X,Y,Z] cm world), rotation ([Pitch,Yaw,Roll] deg), intensity (number, cd for point/spot, lux for directional), color ([R,G,B] 0-1).\n"
			     "Workflow: After spawning, call lighting/build_lighting to bake statics.\n"
			     "Warning: Directional lights should be unique per level."))
		.RequiredEnum  (TEXT("light_type"), {TEXT("point"), TEXT("spot"), TEXT("directional"), TEXT("rect"), TEXT("sky")}, TEXT("Light actor class"))
		.RequiredVec3  (TEXT("location"),   TEXT("World location [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Rotation [Pitch, Yaw, Roll] in degrees"))
		.OptionalNumber(TEXT("intensity"),  TEXT("Intensity. Units depend on light type."))
		.OptionalColor (TEXT("color"),      TEXT("RGB color 0-1, default white"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_intensity"),
			TEXT("Set the intensity of an existing light actor by label.\n"
			     "Params: actor_name (string, actor label), intensity (number, cd for point/spot/rect, lux for directional).\n"
			     "Workflow: Call lighting/build_lighting afterwards for static lights."))
		.RequiredString(TEXT("actor_name"), TEXT("Light actor label"))
		.RequiredNumber(TEXT("intensity"),  TEXT("Intensity value"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_color"),
			TEXT("Set the color of an existing light actor by label.\n"
			     "Params: actor_name (string), color ([R,G,B] 0-1).\n"
			     "Workflow: Call lighting/build_lighting afterwards for static lights."))
		.RequiredString(TEXT("actor_name"), TEXT("Light actor label"))
		.RequiredColor (TEXT("color"),      TEXT("RGB color 0-1"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_attenuation"),
			TEXT("Set attenuation radius (point/spot/rect) and/or cone angles (spot) on a light.\n"
			     "Params: actor_name (string), radius (number, cm, optional), inner_cone_angle (number, deg, spot only), outer_cone_angle (number, deg, spot only). At least one of radius/inner_cone_angle/outer_cone_angle required.\n"
			     "Warning: Radius is not valid for directional/sky lights."))
		.RequiredString (TEXT("actor_name"),        TEXT("Light actor label"))
		.OptionalNumber (TEXT("radius"),            TEXT("Attenuation radius in cm (point/spot/rect only)"))
		.OptionalNumber (TEXT("inner_cone_angle"),  TEXT("Spot light inner cone half-angle in degrees"))
		.OptionalNumber (TEXT("outer_cone_angle"),  TEXT("Spot light outer cone half-angle in degrees"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_cast_shadows"),
			TEXT("Toggle shadow casting on a light actor.\n"
			     "Params: actor_name (string), cast_shadows (bool)."))
		.RequiredString(TEXT("actor_name"),   TEXT("Light actor label"))
		.RequiredBool  (TEXT("cast_shadows"), TEXT("Enable or disable shadow casting"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("build_lighting"),
			TEXT("Trigger an editor lighting build on the current world. Returns when the build is dispatched (bake may continue asynchronously).\n"
			     "Params: none.\n"
			     "Warning: Heavy operation. Ensure no PIE is running."))
		.Build());

	return Tools;
}

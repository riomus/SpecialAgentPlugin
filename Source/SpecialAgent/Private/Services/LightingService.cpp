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

#include "Engine/SphereReflectionCapture.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/PlaneReflectionCapture.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"

#include "EditorBuildUtils.h"
#include "ScopedTransaction.h"

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
	if (MethodName == TEXT("spawn_reflection_capture")) return HandleSpawnReflectionCapture(Request);
	if (MethodName == TEXT("recapture")) return HandleRecapture(Request);

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

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn light")));

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

		NewActor->MarkPackageDirty();

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
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set light intensity")));
		R.BaseComp->Modify();
		if (R.LightComp)         R.LightComp->SetIntensity(IntensityF);
		else if (R.SkyLightComp) R.SkyLightComp->SetIntensity(IntensityF);
		R.BaseComp->MarkPackageDirty();

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
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set light color")));
		R.BaseComp->Modify();
		if (R.LightComp)         R.LightComp->SetLightColor(Color);
		else if (R.SkyLightComp) R.SkyLightComp->SetLightColor(Color);
		R.BaseComp->MarkPackageDirty();

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

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set light attenuation")));

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
					Spot->Modify();
					Spot->SetInnerConeAngle(static_cast<float>(InnerCone));
					Result->SetNumberField(TEXT("inner_cone_angle"), InnerCone);
					bApplied = true;
				}
				if (bHasOuter)
				{
					Spot->Modify();
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
					Local->Modify();
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

		Actor->MarkPackageDirty();
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
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set light cast shadows")));
		R.BaseComp->Modify();
		R.BaseComp->SetCastShadows(bCast);
		R.BaseComp->MarkPackageDirty();

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

	// Lightmass bakes block the game thread and can run well past the default
	// 120s bound on large levels; allow up to 30 minutes before treating the
	// wait as a wedge so a real build is not turned into a false timeout error.
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task, 1800.0);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// spawn_reflection_capture
//   shape sphere -> ASphereReflectionCapture, box -> ABoxReflectionCapture,
//   plane -> APlaneReflectionCapture. influence_radius applies to sphere only
//   (USphereReflectionCaptureComponent::InfluenceRadius); brightness applies to
//   any capture (UReflectionCaptureComponent::Brightness).
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleSpawnReflectionCapture(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString Shape;
	if (!FMCPJson::ReadString(Request.Params, TEXT("shape"), Shape))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'shape' (sphere|box|plane)"));
	}
	Shape = Shape.ToLower();

	FVector Location(0, 0, 0);
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' [X, Y, Z]"));
	}

	double InfluenceRadius = 0.0;
	const bool bHasInfluenceRadius = FMCPJson::ReadNumber(Request.Params, TEXT("influence_radius"), InfluenceRadius);
	double Brightness = 0.0;
	const bool bHasBrightness = FMCPJson::ReadNumber(Request.Params, TEXT("brightness"), Brightness);

	auto Task = [Shape, Location, bHasInfluenceRadius, InfluenceRadius, bHasBrightness, Brightness]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		UClass* SpawnClass = nullptr;
		if      (Shape == TEXT("sphere")) SpawnClass = ASphereReflectionCapture::StaticClass();
		else if (Shape == TEXT("box"))    SpawnClass = ABoxReflectionCapture::StaticClass();
		else if (Shape == TEXT("plane"))  SpawnClass = APlaneReflectionCapture::StaticClass();
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: spawn_reflection_capture unknown shape '%s'"), *Shape);
			return FMCPJson::MakeError(FString::Printf(TEXT("Unknown shape: %s (expected sphere|box|plane)"), *Shape));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn reflection capture")));

		FActorSpawnParameters SpawnParams;
		AActor* NewActor = World->SpawnActor<AActor>(SpawnClass, Location, FRotator::ZeroRotator, SpawnParams);
		if (!NewActor)
		{
			// APlaneReflectionCapture is declared 'abstract' in the engine and cannot be instantiated.
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: spawn_reflection_capture SpawnActor returned null for shape '%s'"), *Shape);
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to spawn reflection capture (shape '%s'); the plane capture class is abstract and cannot be spawned"), *Shape));
		}

		if (AReflectionCapture* Capture = Cast<AReflectionCapture>(NewActor))
		{
			if (UReflectionCaptureComponent* Comp = Capture->GetCaptureComponent())
			{
				bool bModified = false;
				if (bHasInfluenceRadius)
				{
					if (USphereReflectionCaptureComponent* SphereComp = Cast<USphereReflectionCaptureComponent>(Comp))
					{
						const float NewRadius = static_cast<float>(InfluenceRadius);
						if (SphereComp->InfluenceRadius != NewRadius)
						{
							SphereComp->Modify();
							SphereComp->InfluenceRadius = NewRadius;
							bModified = true;
						}
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: spawn_reflection_capture 'influence_radius' ignored for non-sphere shape '%s'"), *Shape);
					}
				}
				if (bHasBrightness)
				{
					const float NewBrightness = static_cast<float>(Brightness);
					if (Comp->Brightness != NewBrightness)
					{
						Comp->Modify();
						Comp->Brightness = NewBrightness;
						bModified = true;
					}
				}
				if (bModified)
				{
					Comp->MarkDirtyForRecapture();
				}
			}
		}

		NewActor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("shape"), Shape);
		TSharedPtr<FJsonObject> ActorData = MakeShared<FJsonObject>();
		FMCPJson::WriteActor(ActorData, NewActor);
		Result->SetObjectField(TEXT("actor"), ActorData);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Spawned %s reflection capture '%s' at (%.1f, %.1f, %.1f)"),
			*Shape, *NewActor->GetActorLabel(), Location.X, Location.Y, Location.Z);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

// -----------------------------------------------------------------------------
// recapture
//   Rebuilds the reflection-capture cubemaps for the editor world so capture
//   edits (new captures, brightness/radius changes) become visible. Uses the
//   engine's static UReflectionCaptureComponent::UpdateReflectionCaptureContents
//   (the same path GEditor->BuildReflectionCaptures dispatches to). GPU-heavy.
// -----------------------------------------------------------------------------

FMCPResponse FLightingService::HandleRecapture(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPJson::MakeError(TEXT("No editor world"));
		}

		UReflectionCaptureComponent::UpdateReflectionCaptureContents(
			World,
			TEXT("SpecialAgent: recapture"),
			/*bVerifyOnlyCapturing=*/ false,
			/*bCapturingForMobile=*/ false);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("note"), TEXT("Reflection capture contents updated for the editor world; GPU-heavy operation that re-renders cubemaps for every reflection capture in the level."));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: recapture updated reflection capture contents"));
		return Result;
	};

	// Recapturing re-renders a cubemap per reflection capture on the GPU and runs
	// on the game thread; allow up to 30 minutes before treating the wait as a
	// wedge so a heavy level is not turned into a false timeout error.
	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task, 1800.0);
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
			TEXT("Spawn a point/spot/directional/rect/sky light actor into the editor world and apply optional intensity/color. "
			     "Returns {success, light_type, actor:{actor_label, ...}}; pass the returned actor_label to the lighting/set_* tools. "
			     "Params: light_type (enum point|spot|directional|rect|sky, required), location (world-space cm [X,Y,Z], required), "
			     "rotation (degrees [pitch,yaw,roll], optional, default 0; matters for directional/spot, ignored for point/sky), "
			     "intensity (number, optional: lux for directional, candela for point/spot/rect, unitless scale for sky, default uses the actor's class default), "
			     "color (linear RGB 0-1 [R,G,B], optional, default white). "
			     "Workflow: spawn the SkyAtmosphere/SkyLight via the sky/* tools first; rotate a directional light with sky/set_sun_angle; only call lighting/build_lighting if the light is Static/Stationary (Lumen needs no bake). "
			     "Warning: spawns into the in-memory level only (not saved to disk); keep directional lights to one per level since each extra one re-lights the scene."))
		.RequiredEnum  (TEXT("light_type"), {TEXT("point"), TEXT("spot"), TEXT("directional"), TEXT("rect"), TEXT("sky")}, TEXT("Light actor class to spawn"))
		.RequiredVec3  (TEXT("location"),   TEXT("World-space spawn location [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Rotation [pitch, yaw, roll] in degrees, default 0 (drives direction for directional/spot)"))
		.OptionalNumber(TEXT("intensity"),  TEXT("Brightness: lux (directional), candela (point/spot/rect), unitless scale (sky); default = class default"))
		.OptionalColor (TEXT("color"),      TEXT("Linear RGB color 0-1, default white [1,1,1]"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_intensity"),
			TEXT("Set the intensity of an existing light actor (resolved by its editor label). "
			     "Works on point/spot/directional/rect lights and on SkyLight. Returns {success, actor_name, intensity}. "
			     "Params: actor_name (string, the actor's editor label, required), "
			     "intensity (number, required: lux for directional, candela for point/spot/rect, unitless scale for sky). "
			     "Workflow: spawn via lighting/spawn_light first; only call lighting/build_lighting afterwards if the light is Static/Stationary. "
			     "Warning: edits the in-memory light component immediately (Lumen/dynamic lights update live) but does not save the level; errors if actor_name is not a light."))
		.RequiredString(TEXT("actor_name"), TEXT("Editor label of the light actor"))
		.RequiredNumber(TEXT("intensity"),  TEXT("Intensity: lux (directional), candela (point/spot/rect), unitless scale (sky)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_color"),
			TEXT("Set the light color (filter tint) of an existing light actor, resolved by its editor label. "
			     "Works on point/spot/directional/rect lights and on SkyLight. Returns {success, actor_name, color:[R,G,B]}. "
			     "Params: actor_name (string, the actor's editor label, required), "
			     "color (linear RGB 0-1 [R,G,B], required; values may exceed 1 for HDR tints). "
			     "Workflow: spawn via lighting/spawn_light first; only call lighting/build_lighting afterwards if the light is Static/Stationary. "
			     "Warning: edits the in-memory light component immediately but does not save the level; errors if actor_name is not a light. This is a color tint, not intensity (use lighting/set_light_intensity for brightness)."))
		.RequiredString(TEXT("actor_name"), TEXT("Editor label of the light actor"))
		.RequiredColor (TEXT("color"),      TEXT("Linear RGB color tint 0-1 [R,G,B] (may exceed 1 for HDR)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_attenuation"),
			TEXT("Set the attenuation radius (point/spot/rect lights) and/or spot-light cone half-angles, on a light resolved by editor label. "
			     "Returns {success, actor_name} plus whichever of {radius, inner_cone_angle, outer_cone_angle} were actually applied. "
			     "Params: actor_name (string, the actor's editor label, required), "
			     "radius (number, cm, optional; attenuation radius for point/spot/rect), "
			     "inner_cone_angle (number, degrees, optional, SPOT ONLY; cone half-angle where falloff begins), "
			     "outer_cone_angle (number, degrees, optional, SPOT ONLY; cone half-angle where the beam ends). "
			     "At least one of radius/inner_cone_angle/outer_cone_angle must be supplied. "
			     "Workflow: spawn the light via lighting/spawn_light, then narrow/widen its reach here. "
			     "Warning: radius applies only to local lights (point/spot/rect) and errors on directional/sky; cone angles are silently ignored on non-spot lights; if no field matches the light type the call returns an error. Edits in-memory only (not saved)."))
		.RequiredString (TEXT("actor_name"),        TEXT("Editor label of the light actor"))
		.OptionalNumber (TEXT("radius"),            TEXT("Attenuation radius in cm (point/spot/rect only)"))
		.OptionalNumber (TEXT("inner_cone_angle"),  TEXT("Spot light inner cone half-angle in degrees (spot only)"))
		.OptionalNumber (TEXT("outer_cone_angle"),  TEXT("Spot light outer cone half-angle in degrees (spot only)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_light_cast_shadows"),
			TEXT("Toggle shadow casting on a light actor, resolved by its editor label (works on point/spot/directional/rect and SkyLight). "
			     "Returns {success, actor_name, cast_shadows}. "
			     "Params: actor_name (string, the actor's editor label, required), cast_shadows (bool, required: true enables, false disables shadows). "
			     "Workflow: spawn via lighting/spawn_light first; only call lighting/build_lighting afterwards if the light is Static/Stationary. "
			     "Warning: dynamic/Movable lights re-evaluate shadows live; Static/Stationary lights need a lighting rebuild. Edits in-memory only (not saved); errors if actor_name is not a light."))
		.RequiredString(TEXT("actor_name"),   TEXT("Editor label of the light actor"))
		.RequiredBool  (TEXT("cast_shadows"), TEXT("true enables shadow casting, false disables it"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("build_lighting"),
			TEXT("Trigger an editor Build Lighting (Lightmass bake) on the current editor world. Returns {success, build_started} when the build is dispatched; "
			     "the bake itself continues asynchronously, so a true result means started, not finished. "
			     "Params: (none). "
			     "Workflow: only useful for Static/Stationary lights with baked lightmaps; spawn/configure lights via lighting/spawn_light and the lighting/set_* tools first. "
			     "Warning: heavy, blocking game-thread Lightmass bake that FREEZES the editor and can take many minutes on large levels (this call waits up to 30 minutes before reporting a timeout). With Lumen (the UE5.7 default GI) enabled this is a NO-OP for diffuse global illumination. GPU Lightmass needs DX12/DXR and is unavailable on macOS/Metal. Do not run while PIE is active."))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_reflection_capture"),
			TEXT("Spawn a reflection-capture actor (sphere/box/plane) into the editor world to provide localized cubemap reflections, and apply optional influence radius (sphere) and brightness. "
			     "Returns {success, shape, actor:{actor_label, ...}}; pass the returned actor_label onward and call lighting/recapture to make the new capture visible. "
			     "Params: shape (enum sphere|box|plane, required), location (world-space cm [X,Y,Z], required), "
			     "influence_radius (number, cm, optional, SPHERE ONLY; radius of the area that receives reflections from this capture, default = class default ~3000), "
			     "brightness (number, optional; scales the captured scene's reflection intensity, UI range ~0.5-4, default = class default 1). "
			     "Workflow: spawn the capture here, then call lighting/recapture to render its cubemap (newly spawned captures show no reflections until recaptured). "
			     "Warning: spawns into the in-memory level only (not saved to disk); influence_radius is ignored on box/plane; the engine plane-capture class is declared abstract and currently fails to spawn (use sphere or box); reflections do not update until you call lighting/recapture."))
		.RequiredEnum  (TEXT("shape"),            {TEXT("sphere"), TEXT("box"), TEXT("plane")}, TEXT("Reflection capture actor shape to spawn"))
		.RequiredVec3  (TEXT("location"),         TEXT("World-space spawn location [X, Y, Z] in cm"))
		.OptionalNumber(TEXT("influence_radius"), TEXT("Sphere capture influence radius in cm (sphere only); default = class default"))
		.OptionalNumber(TEXT("brightness"),       TEXT("Reflection brightness multiplier (UI range ~0.5-4); default = class default 1"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("recapture"),
			TEXT("Rebuild (recapture) the reflection-capture cubemap contents for the current editor world so capture edits become visible (newly spawned captures, brightness/influence_radius changes). "
			     "Returns {success, note}. Calls the engine's UpdateReflectionCaptureContents path (the same one the editor's Build Reflection Captures uses) on the editor world. "
			     "Params: (none). "
			     "Workflow: spawn or edit captures via lighting/spawn_reflection_capture (and any reflection-capture property tools) first, then call this once to render their cubemaps; a single recapture refreshes every dirty capture in the level. "
			     "Warning: GPU-heavy, blocking game-thread operation that re-renders a cubemap for every queued reflection capture and can take many minutes on large levels (this call waits up to 30 minutes before reporting a timeout). Do not run while PIE is active."))
		.Build());

	return Tools;
}

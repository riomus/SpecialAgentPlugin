// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/GameplayService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/TriggerVolume.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Note.h"
#include "Engine/TargetPoint.h"
#include "GameFramework/KillZVolume.h"
#include "GameFramework/PlayerStart.h"

FGameplayService::FGameplayService()
{
}

FString FGameplayService::GetServiceDescription() const
{
	return TEXT("Gameplay actor management - spawn trigger volumes, player starts, notes, target points, kill-Z volumes, and blocking volumes.");
}

namespace
{
	// Default Unreal volume is a 200x200x200 cube (extent 100 on each axis).
	// Scale by ExtentCm/DefaultExtent to reach the requested extent in cm.
	constexpr float DefaultVolumeExtent = 100.0f;

	// Shared spawn helper. Always runs on the game thread.
	template<typename TActor>
	TSharedPtr<FJsonObject> SpawnBasicActor(const FVector& Location,
	                                        const FRotator& Rotation,
	                                        const FString& OptionalLabel,
	                                        TFunction<void(TActor*)> Configure = nullptr)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world"));
			return Result;
		}

		FActorSpawnParameters SpawnParams;
		TActor* NewActor = World->SpawnActor<TActor>(TActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
		if (!NewActor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("SpawnActor returned null"));
			return Result;
		}

		// Rotation applied after spawn to avoid volume collision-initialization quirks.
		if (!Rotation.IsNearlyZero())
		{
			NewActor->SetActorRotation(Rotation);
		}

		if (Configure)
		{
			Configure(NewActor);
		}

		if (!OptionalLabel.IsEmpty())
		{
			NewActor->SetActorLabel(OptionalLabel);
		}

		Result->SetBoolField(TEXT("success"), true);
		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		FMCPJson::WriteActor(ActorObj, NewActor);
		Result->SetObjectField(TEXT("actor"), ActorObj);
		return Result;
	}
}

FMCPResponse FGameplayService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("spawn_trigger_volume"))  return HandleSpawnTriggerVolume(Request);
	if (MethodName == TEXT("spawn_player_start"))    return HandleSpawnPlayerStart(Request);
	if (MethodName == TEXT("spawn_note"))            return HandleSpawnNote(Request);
	if (MethodName == TEXT("spawn_target_point"))    return HandleSpawnTargetPoint(Request);
	if (MethodName == TEXT("spawn_killz_volume"))    return HandleSpawnKillZVolume(Request);
	if (MethodName == TEXT("spawn_blocking_volume")) return HandleSpawnBlockingVolume(Request);

	return MethodNotFound(Request.Id, TEXT("gameplay"), MethodName);
}

// --- Helpers shared by handlers ----------------------------------------------

static bool ReadSpawnBasics(const FMCPRequest& Request, FString& OutError,
                            FVector& OutLocation, FRotator& OutRotation, FString& OutLabel)
{
	OutLocation = FVector::ZeroVector;
	OutRotation = FRotator::ZeroRotator;
	OutLabel.Reset();

	if (!Request.Params.IsValid())
	{
		OutError = TEXT("Missing params");
		return false;
	}

	if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), OutLocation))
	{
		OutError = TEXT("Missing or invalid 'location' [X, Y, Z]");
		return false;
	}

	// Rotation optional.
	FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), OutRotation);

	// Label optional.
	FMCPJson::ReadString(Request.Params, TEXT("label"), OutLabel);

	return true;
}

// --- Handlers ----------------------------------------------------------------

FMCPResponse FGameplayService::HandleSpawnTriggerVolume(const FMCPRequest& Request)
{
	FString Error;
	FVector Location;
	FRotator Rotation;
	FString Label;
	if (!ReadSpawnBasics(Request, Error, Location, Rotation, Label))
	{
		return InvalidParams(Request.Id, Error);
	}

	// Optional box extent (cm, half-size). Default volume is already 100,100,100.
	FVector BoxExtent(DefaultVolumeExtent, DefaultVolumeExtent, DefaultVolumeExtent);
	const bool bHasExtent = FMCPJson::ReadVec3(Request.Params, TEXT("box_extent"), BoxExtent);

	auto Task = [Location, Rotation, Label, BoxExtent, bHasExtent]() -> TSharedPtr<FJsonObject>
	{
		auto Configure = [BoxExtent, bHasExtent](ATriggerVolume* Volume)
		{
			if (bHasExtent)
			{
				// Volumes are built from a unit-cube brush; resize by scaling the actor
				// so the collision shape matches the requested extent.
				const FVector Scale(
					BoxExtent.X / DefaultVolumeExtent,
					BoxExtent.Y / DefaultVolumeExtent,
					BoxExtent.Z / DefaultVolumeExtent);
				Volume->SetActorScale3D(Scale);
			}
		};
		return SpawnBasicActor<ATriggerVolume>(Location, Rotation, Label, Configure);
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	if (Result->GetBoolField(TEXT("success")))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: gameplay/spawn_trigger_volume at (%.1f, %.1f, %.1f)"),
			Location.X, Location.Y, Location.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: gameplay/spawn_trigger_volume failed: %s"),
			*Result->GetStringField(TEXT("error")));
	}
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FGameplayService::HandleSpawnPlayerStart(const FMCPRequest& Request)
{
	FString Error;
	FVector Location;
	FRotator Rotation;
	FString Label;
	if (!ReadSpawnBasics(Request, Error, Location, Rotation, Label))
	{
		return InvalidParams(Request.Id, Error);
	}

	auto Task = [Location, Rotation, Label]() -> TSharedPtr<FJsonObject>
	{
		return SpawnBasicActor<APlayerStart>(Location, Rotation, Label);
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	if (Result->GetBoolField(TEXT("success")))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: gameplay/spawn_player_start at (%.1f, %.1f, %.1f)"),
			Location.X, Location.Y, Location.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: gameplay/spawn_player_start failed: %s"),
			*Result->GetStringField(TEXT("error")));
	}
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FGameplayService::HandleSpawnNote(const FMCPRequest& Request)
{
	FString Error;
	FVector Location;
	FRotator Rotation;
	FString Label;
	if (!ReadSpawnBasics(Request, Error, Location, Rotation, Label))
	{
		return InvalidParams(Request.Id, Error);
	}

	FString NoteText;
	FMCPJson::ReadString(Request.Params, TEXT("text"), NoteText);

	auto Task = [Location, Rotation, Label, NoteText]() -> TSharedPtr<FJsonObject>
	{
		auto Configure = [NoteText](ANote* Note)
		{
#if WITH_EDITORONLY_DATA
			if (!NoteText.IsEmpty())
			{
				Note->Text = NoteText;
			}
#endif
		};
		return SpawnBasicActor<ANote>(Location, Rotation, Label, Configure);
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	if (Result->GetBoolField(TEXT("success")))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: gameplay/spawn_note (%d chars) at (%.1f, %.1f, %.1f)"),
			NoteText.Len(), Location.X, Location.Y, Location.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: gameplay/spawn_note failed: %s"),
			*Result->GetStringField(TEXT("error")));
	}
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FGameplayService::HandleSpawnTargetPoint(const FMCPRequest& Request)
{
	FString Error;
	FVector Location;
	FRotator Rotation;
	FString Label;
	if (!ReadSpawnBasics(Request, Error, Location, Rotation, Label))
	{
		return InvalidParams(Request.Id, Error);
	}

	auto Task = [Location, Rotation, Label]() -> TSharedPtr<FJsonObject>
	{
		return SpawnBasicActor<ATargetPoint>(Location, Rotation, Label);
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	if (Result->GetBoolField(TEXT("success")))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: gameplay/spawn_target_point at (%.1f, %.1f, %.1f)"),
			Location.X, Location.Y, Location.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: gameplay/spawn_target_point failed: %s"),
			*Result->GetStringField(TEXT("error")));
	}
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FGameplayService::HandleSpawnKillZVolume(const FMCPRequest& Request)
{
	FString Error;
	FVector Location;
	FRotator Rotation;
	FString Label;
	if (!ReadSpawnBasics(Request, Error, Location, Rotation, Label))
	{
		return InvalidParams(Request.Id, Error);
	}

	FVector BoxExtent(DefaultVolumeExtent, DefaultVolumeExtent, DefaultVolumeExtent);
	const bool bHasExtent = FMCPJson::ReadVec3(Request.Params, TEXT("box_extent"), BoxExtent);

	auto Task = [Location, Rotation, Label, BoxExtent, bHasExtent]() -> TSharedPtr<FJsonObject>
	{
		auto Configure = [BoxExtent, bHasExtent](AKillZVolume* Volume)
		{
			if (bHasExtent)
			{
				const FVector Scale(
					BoxExtent.X / DefaultVolumeExtent,
					BoxExtent.Y / DefaultVolumeExtent,
					BoxExtent.Z / DefaultVolumeExtent);
				Volume->SetActorScale3D(Scale);
			}
		};
		return SpawnBasicActor<AKillZVolume>(Location, Rotation, Label, Configure);
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	if (Result->GetBoolField(TEXT("success")))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: gameplay/spawn_killz_volume at (%.1f, %.1f, %.1f)"),
			Location.X, Location.Y, Location.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: gameplay/spawn_killz_volume failed: %s"),
			*Result->GetStringField(TEXT("error")));
	}
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FGameplayService::HandleSpawnBlockingVolume(const FMCPRequest& Request)
{
	FString Error;
	FVector Location;
	FRotator Rotation;
	FString Label;
	if (!ReadSpawnBasics(Request, Error, Location, Rotation, Label))
	{
		return InvalidParams(Request.Id, Error);
	}

	FVector BoxExtent(DefaultVolumeExtent, DefaultVolumeExtent, DefaultVolumeExtent);
	const bool bHasExtent = FMCPJson::ReadVec3(Request.Params, TEXT("box_extent"), BoxExtent);

	auto Task = [Location, Rotation, Label, BoxExtent, bHasExtent]() -> TSharedPtr<FJsonObject>
	{
		auto Configure = [BoxExtent, bHasExtent](ABlockingVolume* Volume)
		{
			if (bHasExtent)
			{
				const FVector Scale(
					BoxExtent.X / DefaultVolumeExtent,
					BoxExtent.Y / DefaultVolumeExtent,
					BoxExtent.Z / DefaultVolumeExtent);
				Volume->SetActorScale3D(Scale);
			}
		};
		return SpawnBasicActor<ABlockingVolume>(Location, Rotation, Label, Configure);
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	if (Result->GetBoolField(TEXT("success")))
	{
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: gameplay/spawn_blocking_volume at (%.1f, %.1f, %.1f)"),
			Location.X, Location.Y, Location.Z);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: gameplay/spawn_blocking_volume failed: %s"),
			*Result->GetStringField(TEXT("error")));
	}
	return FMCPResponse::Success(Request.Id, Result);
}

// --- Tool schemas ------------------------------------------------------------

TArray<FMCPToolInfo> FGameplayService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_trigger_volume"),
			TEXT("Spawn a trigger volume. Places an ATriggerVolume (AVolume-derived box trigger) in the level. "
			     "Params: location ([X,Y,Z] cm, world), rotation ([Pitch,Yaw,Roll] deg, optional), box_extent ([X,Y,Z] cm half-size, optional — applied via actor scale; default 100,100,100), label (string, optional actor label). "
			     "Workflow: use viewport/trace_from_screen for placement, then world/list_actors to confirm. "
			     "Warning: extent is mapped onto ActorScale3D — the volume's brush stays unit-size."))
		.RequiredVec3  (TEXT("location"),   TEXT("Spawn location as [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Optional rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalVec3  (TEXT("box_extent"), TEXT("Optional half-extent as [X, Y, Z] in cm; default 100,100,100"))
		.OptionalString(TEXT("label"),      TEXT("Optional actor label"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_player_start"),
			TEXT("Spawn a player start. Places an APlayerStart — the default spawn location for Pawns at PIE/game start. "
			     "Params: location ([X,Y,Z] cm, world), rotation ([Pitch,Yaw,Roll] deg, optional — determines initial facing), label (string, optional). "
			     "Workflow: place on walkable ground; use viewport/trace_from_screen to snap to geometry. "
			     "Warning: if multiple player starts exist, UE chooses one via GameMode rules — label them to disambiguate."))
		.RequiredVec3  (TEXT("location"), TEXT("Spawn location as [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"), TEXT("Optional rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalString(TEXT("label"),    TEXT("Optional actor label"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_note"),
			TEXT("Spawn an editor note. Places an ANote — an editor-only sticky-note actor used to flag design tasks. "
			     "Params: location ([X,Y,Z] cm), rotation ([Pitch,Yaw,Roll] deg, optional), text (string, optional note body), label (string, optional). "
			     "Workflow: use to annotate level review findings; visible only in editor. "
			     "Warning: text is stripped from cooked builds (WITH_EDITORONLY_DATA)."))
		.RequiredVec3  (TEXT("location"), TEXT("Spawn location as [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"), TEXT("Optional rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalString(TEXT("text"),     TEXT("Optional note body text"))
		.OptionalString(TEXT("label"),    TEXT("Optional actor label"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_target_point"),
			TEXT("Spawn a target point. Places an ATargetPoint — a lightweight transform marker for AI, cinematics, or pathing hooks. "
			     "Params: location ([X,Y,Z] cm), rotation ([Pitch,Yaw,Roll] deg, optional — encodes facing/vector for consumers), label (string, optional). "
			     "Workflow: reference target points by label from Blueprints or behavior trees. "
			     "Warning: has no runtime logic — it's a pure transform bearer."))
		.RequiredVec3  (TEXT("location"), TEXT("Spawn location as [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"), TEXT("Optional rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalString(TEXT("label"),    TEXT("Optional actor label"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_killz_volume"),
			TEXT("Spawn a kill-Z volume. Places an AKillZVolume (PhysicsVolume-derived) that destroys any actor entering it by calling FellOutOfWorld. "
			     "Params: location ([X,Y,Z] cm — volume center), rotation ([Pitch,Yaw,Roll] deg, optional), box_extent ([X,Y,Z] cm half-size, optional — via actor scale; default 100,100,100), label (string, optional). "
			     "Workflow: place below playable area to catch fallen actors. "
			     "Warning: destroys ANY pawn/actor overlapping it; verify extent before stretching across a level."))
		.RequiredVec3  (TEXT("location"),   TEXT("Spawn location as [X, Y, Z] in cm (volume center)"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Optional rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalVec3  (TEXT("box_extent"), TEXT("Optional half-extent as [X, Y, Z] in cm; default 100,100,100"))
		.OptionalString(TEXT("label"),      TEXT("Optional actor label"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_blocking_volume"),
			TEXT("Spawn a blocking volume. Places an ABlockingVolume — an invisible collider that blocks pawns and physics but has no rendering cost. "
			     "Params: location ([X,Y,Z] cm — volume center), rotation ([Pitch,Yaw,Roll] deg, optional), box_extent ([X,Y,Z] cm half-size, optional — via actor scale; default 100,100,100), label (string, optional). "
			     "Workflow: use for invisible walls on ledges or level boundaries. "
			     "Warning: still collides in cooked builds; exclude from streaming only if you mean it."))
		.RequiredVec3  (TEXT("location"),   TEXT("Spawn location as [X, Y, Z] in cm (volume center)"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Optional rotation as [Pitch, Yaw, Roll] in degrees"))
		.OptionalVec3  (TEXT("box_extent"), TEXT("Optional half-extent as [X, Y, Z] in cm; default 100,100,100"))
		.OptionalString(TEXT("label"),      TEXT("Optional actor label"))
		.Build());

	return Tools;
}

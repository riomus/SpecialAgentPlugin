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
#include "ScopedTransaction.h"

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

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn actor")));

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

		// Flag the owning level package dirty so the new actor is saveable.
		NewActor->MarkPackageDirty();

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
			TEXT("Spawn an ATriggerVolume (box trigger) into the persistent editor world. Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}}. "
			     "Params: location ([X,Y,Z] world-space cm, required — volume center), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0]), box_extent ([X,Y,Z] cm half-extent, optional, default [100,100,100]), label (string, optional actor label). "
			     "Workflow: use viewport/trace_from_screen to pick a location, then world/list_actors to confirm by the returned label. "
			     "Warning: in-memory only — NOT saved to disk (save the level package to persist). box_extent is applied via ActorScale3D (extent/100), so the underlying brush stays unit-size; default volume is a 200 cm cube (extent 100)."))
		.RequiredVec3  (TEXT("location"),   TEXT("Volume center as world-space [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Rotation as [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalVec3  (TEXT("box_extent"), TEXT("Half-extent as [X, Y, Z] in cm, applied via actor scale (default [100,100,100])"))
		.OptionalString(TEXT("label"),      TEXT("Actor label shown in the World Outliner (optional)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_player_start"),
			TEXT("Spawn an APlayerStart (the default Pawn spawn point at PIE/game start) into the editor world. Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}}. "
			     "Params: location ([X,Y,Z] world-space cm, required), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0] — sets the Pawn's initial facing; yaw is the heading), label (string, optional actor label). "
			     "Workflow: place on walkable ground; use viewport/trace_from_screen to snap location to geometry, then world/list_actors to confirm. "
			     "Warning: in-memory only — NOT saved to disk until you save the level. With multiple player starts the GameMode picks one; set distinct labels to disambiguate."))
		.RequiredVec3  (TEXT("location"), TEXT("Spawn location as world-space [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"), TEXT("Initial facing as [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalString(TEXT("label"),    TEXT("Actor label shown in the World Outliner (optional)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_note"),
			TEXT("Spawn an ANote (editor-only sticky-note actor) used to flag design tasks. Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}}. "
			     "Params: location ([X,Y,Z] world-space cm, required), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0]), text (string, optional — the note body, applied to ANote.Text), label (string, optional actor label). "
			     "Workflow: use to annotate level-review findings; visible only in the editor. "
			     "Warning: in-memory only — NOT saved to disk until you save the level. The note text lives behind WITH_EDITORONLY_DATA and is stripped from cooked builds."))
		.RequiredVec3  (TEXT("location"), TEXT("Spawn location as world-space [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"), TEXT("Rotation as [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalString(TEXT("text"),     TEXT("Note body text written to ANote.Text (optional)"))
		.OptionalString(TEXT("label"),    TEXT("Actor label shown in the World Outliner (optional)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_target_point"),
			TEXT("Spawn an ATargetPoint (lightweight transform marker) for AI, cinematics, or pathing hooks. Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}}. "
			     "Params: location ([X,Y,Z] world-space cm, required), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0] — encodes a facing/vector for consumers), label (string, optional actor label). "
			     "Workflow: reference target points by label from Blueprints or behavior trees. "
			     "Warning: in-memory only — NOT saved to disk until you save the level. Has no runtime logic; it is a pure transform bearer."))
		.RequiredVec3  (TEXT("location"), TEXT("Spawn location as world-space [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"), TEXT("Facing as [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalString(TEXT("label"),    TEXT("Actor label shown in the World Outliner (optional)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_killz_volume"),
			TEXT("Spawn an AKillZVolume (PhysicsVolume-derived) that destroys actors entering it (FellOutOfWorld). Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}}. "
			     "Params: location ([X,Y,Z] world-space cm, required — volume center), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0]), box_extent ([X,Y,Z] cm half-extent, optional, default [100,100,100]), label (string, optional actor label). "
			     "Workflow: place below the playable area to catch fallen actors at runtime. "
			     "Warning: in-memory only — NOT saved to disk until you save the level. box_extent is applied via ActorScale3D (extent/100); destroys ANY overlapping pawn/actor in PIE/game, so size it deliberately before stretching across the level."))
		.RequiredVec3  (TEXT("location"),   TEXT("Volume center as world-space [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Rotation as [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalVec3  (TEXT("box_extent"), TEXT("Half-extent as [X, Y, Z] in cm, applied via actor scale (default [100,100,100])"))
		.OptionalString(TEXT("label"),      TEXT("Actor label shown in the World Outliner (optional)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("spawn_blocking_volume"),
			TEXT("Spawn an ABlockingVolume (invisible collider) that blocks pawns and physics with no rendering cost. Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}}. "
			     "Params: location ([X,Y,Z] world-space cm, required — volume center), rotation ([Pitch,Yaw,Roll] degrees, optional, default [0,0,0]), box_extent ([X,Y,Z] cm half-extent, optional, default [100,100,100]), label (string, optional actor label). "
			     "Workflow: use for invisible walls on ledges or level boundaries. "
			     "Warning: in-memory only — NOT saved to disk until you save the level. box_extent is applied via ActorScale3D (extent/100); collision persists in cooked builds."))
		.RequiredVec3  (TEXT("location"),   TEXT("Volume center as world-space [X, Y, Z] in cm"))
		.OptionalVec3  (TEXT("rotation"),   TEXT("Rotation as [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
		.OptionalVec3  (TEXT("box_extent"), TEXT("Half-extent as [X, Y, Z] in cm, applied via actor scale (default [100,100,100])"))
		.OptionalString(TEXT("label"),      TEXT("Actor label shown in the World Outliner (optional)"))
		.Build());

	return Tools;
}

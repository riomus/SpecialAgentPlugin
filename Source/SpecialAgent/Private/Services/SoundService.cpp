// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/SoundService.h"
#include "MCPCommon/MCPRequestContext.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundBase.h"

FString FSoundService::GetServiceDescription() const
{
    return TEXT("Sound playback and ambient sound actors");
}

FMCPResponse FSoundService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("play_2d"))              return HandlePlay2D(Request);
    if (MethodName == TEXT("play_at_location"))     return HandlePlayAtLocation(Request);
    if (MethodName == TEXT("spawn_ambient_actor"))  return HandleSpawnAmbientActor(Request);
    if (MethodName == TEXT("set_volume_multiplier"))return HandleSetVolumeMultiplier(Request);

    return MethodNotFound(Request.Id, TEXT("sound"), MethodName);
}

FMCPResponse FSoundService::HandlePlay2D(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString SoundPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sound"), SoundPath) || SoundPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'sound' (asset path to USoundBase)"));

    double VolumeMultiplier = 1.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("volume_multiplier"), VolumeMultiplier);

    double PitchMultiplier = 1.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("pitch_multiplier"), PitchMultiplier);

    double StartTime = 0.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("start_time"), StartTime);

    auto Task = [SoundPath, VolumeMultiplier, PitchMultiplier, StartTime]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        USoundBase* Sound = LoadObject<USoundBase>(nullptr, *SoundPath);
        if (!Sound)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Failed to load USoundBase: %s"), *SoundPath));
        }

        UGameplayStatics::PlaySound2D(World, Sound,
            static_cast<float>(VolumeMultiplier),
            static_cast<float>(PitchMultiplier),
            static_cast<float>(StartTime));

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("sound"),             Sound->GetPathName());
        Result->SetNumberField(TEXT("volume_multiplier"), VolumeMultiplier);
        Result->SetNumberField(TEXT("pitch_multiplier"),  PitchMultiplier);
        Result->SetNumberField(TEXT("start_time"),        StartTime);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PlaySound2D '%s' (vol=%.2f, pitch=%.2f)"),
            *Sound->GetName(), static_cast<float>(VolumeMultiplier), static_cast<float>(PitchMultiplier));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSoundService::HandlePlayAtLocation(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString SoundPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sound"), SoundPath) || SoundPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'sound' (asset path to USoundBase)"));

    FVector Location(0, 0, 0);
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' ([X, Y, Z])"));

    FRotator Rotation(0, 0, 0);
    FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

    double VolumeMultiplier = 1.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("volume_multiplier"), VolumeMultiplier);

    double PitchMultiplier = 1.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("pitch_multiplier"), PitchMultiplier);

    double StartTime = 0.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("start_time"), StartTime);

    auto Task = [SoundPath, Location, Rotation, VolumeMultiplier, PitchMultiplier, StartTime]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        USoundBase* Sound = LoadObject<USoundBase>(nullptr, *SoundPath);
        if (!Sound)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Failed to load USoundBase: %s"), *SoundPath));
        }

        UGameplayStatics::PlaySoundAtLocation(World, Sound, Location, Rotation,
            static_cast<float>(VolumeMultiplier),
            static_cast<float>(PitchMultiplier),
            static_cast<float>(StartTime));

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("sound"), Sound->GetPathName());
        FMCPJson::WriteVec3   (Result, TEXT("location"), Location);
        FMCPJson::WriteRotator(Result, TEXT("rotation"), Rotation);
        Result->SetNumberField(TEXT("volume_multiplier"), VolumeMultiplier);
        Result->SetNumberField(TEXT("pitch_multiplier"),  PitchMultiplier);
        Result->SetNumberField(TEXT("start_time"),        StartTime);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PlaySoundAtLocation '%s' at (%.1f, %.1f, %.1f)"),
            *Sound->GetName(), Location.X, Location.Y, Location.Z);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSoundService::HandleSpawnAmbientActor(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString SoundPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sound"), SoundPath) || SoundPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'sound' (asset path to USoundBase)"));

    FVector Location(0, 0, 0);
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' ([X, Y, Z])"));

    FRotator Rotation(0, 0, 0);
    FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

    double VolumeMultiplier = 1.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("volume_multiplier"), VolumeMultiplier);

    auto Task = [SoundPath, Location, Rotation, VolumeMultiplier]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        USoundBase* Sound = LoadObject<USoundBase>(nullptr, *SoundPath);
        if (!Sound)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Failed to load USoundBase: %s"), *SoundPath));
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AAmbientSound* Ambient = World->SpawnActor<AAmbientSound>(
            AAmbientSound::StaticClass(), Location, Rotation, SpawnParams);
        if (!Ambient)
        {
            return FMCPJson::MakeError(TEXT("SpawnActor<AAmbientSound> returned null"));
        }

        if (UAudioComponent* AudioComp = Ambient->GetAudioComponent())
        {
            AudioComp->SetSound(Sound);
            if (VolumeMultiplier != 1.0)
            {
                AudioComp->SetVolumeMultiplier(static_cast<float>(VolumeMultiplier));
            }
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        TSharedPtr<FJsonObject> ActorData = MakeShared<FJsonObject>();
        FMCPJson::WriteActor(ActorData, Ambient);
        Result->SetObjectField(TEXT("actor"), ActorData);
        Result->SetStringField(TEXT("sound"), Sound->GetPathName());
        Result->SetNumberField(TEXT("volume_multiplier"), VolumeMultiplier);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Spawned AmbientSound '%s' with sound '%s'"),
            *Ambient->GetActorLabel(), *Sound->GetName());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSoundService::HandleSetVolumeMultiplier(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name' (AAmbientSound label)"));

    double VolumeMultiplier = 1.0;
    if (!FMCPJson::ReadNumber(Request.Params, TEXT("volume_multiplier"), VolumeMultiplier))
        return InvalidParams(Request.Id, TEXT("Missing 'volume_multiplier' (number)"));

    auto Task = [ActorName, VolumeMultiplier]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        AAmbientSound* Ambient = Cast<AAmbientSound>(Actor);
        if (!Ambient)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Actor '%s' not found or is not an AAmbientSound"), *ActorName));
        }

        UAudioComponent* AudioComp = Ambient->GetAudioComponent();
        if (!AudioComp)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("AmbientSound '%s' has no AudioComponent"), *ActorName));
        }

        AudioComp->SetVolumeMultiplier(static_cast<float>(VolumeMultiplier));

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor"),             Ambient->GetActorLabel());
        Result->SetNumberField(TEXT("volume_multiplier"), VolumeMultiplier);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set AmbientSound '%s' volume_multiplier=%.2f"),
            *Ambient->GetActorLabel(), static_cast<float>(VolumeMultiplier));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FSoundService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("play_2d"),
            TEXT("Play a sound non-spatialized (UI-style, no 3D attenuation) via PlaySound2D. Fire-and-forget; returns "
                 "{sound, volume_multiplier, pitch_multiplier, start_time}. "
                 "Params: sound (string, required, /Game/... object path to a USoundBase - SoundWave, SoundCue, or MetaSound), "
                 "volume_multiplier (number, optional, linear, 1.0 nominal, default 1.0), "
                 "pitch_multiplier (number, optional, linear, 1.0 nominal, default 1.0), "
                 "start_time (number, optional, seconds offset into the sound, default 0). "
                 "Workflow: use play_at_location for spatialized 3D playback. "
                 "Warning: fire-and-forget - no handle is returned, so it cannot be stopped, faded, or re-parameterized; "
                 "for runtime control spawn a controllable AudioComponent (e.g. spawn_ambient_actor) or use MetaSound parameters. "
                 "Editor-preview playback works only while the editor world is active and is not persisted."))
        .RequiredString(TEXT("sound"),             TEXT("/Game/... object path to a USoundBase (Wave/Cue/MetaSound), e.g. /Game/Sounds/UI/Click.Click"))
        .OptionalNumber(TEXT("volume_multiplier"), TEXT("Linear volume scalar, 1.0 nominal (default 1.0)"))
        .OptionalNumber(TEXT("pitch_multiplier"),  TEXT("Linear pitch scalar, 1.0 nominal (default 1.0)"))
        .OptionalNumber(TEXT("start_time"),        TEXT("Offset into the sound in seconds (default 0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("play_at_location"),
            TEXT("Play a spatialized (3D, attenuated) sound at a fixed world location via PlaySoundAtLocation. Fire-and-forget; "
                 "returns {sound, location, rotation, volume_multiplier, pitch_multiplier, start_time}. "
                 "Params: sound (string, required, /Game/... object path to a USoundBase), "
                 "location (array [X,Y,Z], required, world cm), rotation (array [Pitch,Yaw,Roll], optional, degrees), "
                 "volume_multiplier (number, optional, linear, default 1.0), pitch_multiplier (number, optional, linear, default 1.0), "
                 "start_time (number, optional, seconds, default 0). "
                 "Workflow: use play_2d for non-spatialized UI sounds; use spawn_ambient_actor for a placed, controllable, looping source. "
                 "Warning: fire-and-forget one-shot pinned to the given transform - it does NOT follow any actor and cannot be "
                 "stopped, faded, or re-parameterized after it starts. Editor-preview playback works only while the editor world is active."))
        .RequiredString(TEXT("sound"),             TEXT("/Game/... object path to a USoundBase (Wave/Cue/MetaSound)"))
        .RequiredVec3  (TEXT("location"),          TEXT("World location [X, Y, Z] in cm"))
        .OptionalVec3  (TEXT("rotation"),          TEXT("World rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalNumber(TEXT("volume_multiplier"), TEXT("Linear volume scalar, 1.0 nominal (default 1.0)"))
        .OptionalNumber(TEXT("pitch_multiplier"),  TEXT("Linear pitch scalar, 1.0 nominal (default 1.0)"))
        .OptionalNumber(TEXT("start_time"),        TEXT("Offset into the sound in seconds (default 0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("spawn_ambient_actor"),
            TEXT("Spawn an AAmbientSound actor at a world location, assign its sound, and optionally set its initial volume. "
                 "Unlike the fire-and-forget play tools this gives a placed, persistent, controllable source. "
                 "Returns {actor (label/path/transform), sound, volume_multiplier}. "
                 "Params: sound (string, required, /Game/... object path to a USoundBase; a looping Cue/Wave/MetaSound is typical), "
                 "location (array [X,Y,Z], required, world cm), rotation (array [Pitch,Yaw,Roll], optional, degrees), "
                 "volume_multiplier (number, optional, linear, default 1.0; only applied when not 1.0). "
                 "Workflow: spawn_ambient_actor -> set_volume_multiplier to adjust later without respawning. "
                 "Warning: the actor is placed persistently in the level (save the level to keep it); remove it with the "
                 "world delete-actor tool. AAmbientSound auto-plays its sound when the world is active."))
        .RequiredString(TEXT("sound"),             TEXT("/Game/... object path to a USoundBase (looping Cue/Wave/MetaSound typical)"))
        .RequiredVec3  (TEXT("location"),          TEXT("World location [X, Y, Z] in cm"))
        .OptionalVec3  (TEXT("rotation"),          TEXT("World rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalNumber(TEXT("volume_multiplier"), TEXT("Initial linear volume on the AudioComponent (default 1.0, applied only when != 1.0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_volume_multiplier"),
            TEXT("Set the volume multiplier on an existing AAmbientSound actor's AudioComponent. Returns {actor (label), volume_multiplier}. "
                 "Params: actor_name (string, required, editor label of the AAmbientSound), "
                 "volume_multiplier (number, required, linear; 0 = silent, 1.0 = nominal). "
                 "Workflow: call spawn_ambient_actor first to create the target, then adjust here without respawning. "
                 "Warning: targets AAmbientSound actors only - errors if the label resolves to a different actor type or has no "
                 "AudioComponent. For a generic actor, set the AudioComponent volume directly from Python instead."))
        .RequiredString(TEXT("actor_name"),        TEXT("Editor label of the AAmbientSound to adjust"))
        .RequiredNumber(TEXT("volume_multiplier"), TEXT("New linear volume multiplier (0 = silent, 1.0 = nominal)"))
        .Build());

    return Tools;
}

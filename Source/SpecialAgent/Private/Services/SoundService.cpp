// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/SoundService.h"

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

FMCPResponse FSoundService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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
            TEXT("Play a sound non-spatialized (UI-style) via UGameplayStatics::PlaySound2D. Fire-and-forget. "
                 "Params: sound (string, asset path to USoundBase), volume_multiplier (number, default 1.0), pitch_multiplier (number, default 1.0), start_time (number, seconds, default 0). "
                 "Workflow: use sound/play_at_location for spatialized 3D playback. "
                 "Warning: only plays in editor preview while the editor world is active; no persistence."))
        .RequiredString(TEXT("sound"),             TEXT("Asset path to USoundBase (e.g. /Game/Sounds/UI/Click.Click)"))
        .OptionalNumber(TEXT("volume_multiplier"), TEXT("Linear volume scalar (default 1.0)"))
        .OptionalNumber(TEXT("pitch_multiplier"),  TEXT("Linear pitch scalar (default 1.0)"))
        .OptionalNumber(TEXT("start_time"),        TEXT("Offset into the sound in seconds (default 0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("play_at_location"),
            TEXT("Play a spatialized sound at a world location via UGameplayStatics::PlaySoundAtLocation. Fire-and-forget. "
                 "Params: sound (string, asset path), location ([X,Y,Z] cm world), rotation (optional [Pitch,Yaw,Roll] deg), volume_multiplier/pitch_multiplier/start_time (numbers). "
                 "Workflow: use assets/get_bounds + viewport/trace_from_screen to find placement points. "
                 "Warning: does not travel with any actor — the sound is a one-shot at the given transform."))
        .RequiredString(TEXT("sound"),             TEXT("Asset path to USoundBase"))
        .RequiredVec3  (TEXT("location"),          TEXT("World location [X, Y, Z] in cm"))
        .OptionalVec3  (TEXT("rotation"),          TEXT("World rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalNumber(TEXT("volume_multiplier"), TEXT("Linear volume scalar (default 1.0)"))
        .OptionalNumber(TEXT("pitch_multiplier"),  TEXT("Linear pitch scalar (default 1.0)"))
        .OptionalNumber(TEXT("start_time"),        TEXT("Offset into the sound in seconds (default 0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("spawn_ambient_actor"),
            TEXT("Spawn an AAmbientSound actor, assign its sound, and optionally set its volume multiplier. "
                 "Params: sound (string, asset path), location ([X,Y,Z] cm world), rotation (optional), volume_multiplier (number, default 1.0). "
                 "Workflow: use sound/set_volume_multiplier afterwards to adjust without respawning. "
                 "Warning: ambient actors are placed persistently in the level and should be cleaned up via world/delete_actor."))
        .RequiredString(TEXT("sound"),             TEXT("Asset path to USoundBase (a looping SoundCue / SoundWave is typical)"))
        .RequiredVec3  (TEXT("location"),          TEXT("World location [X, Y, Z] in cm"))
        .OptionalVec3  (TEXT("rotation"),          TEXT("World rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalNumber(TEXT("volume_multiplier"), TEXT("Initial volume multiplier on the AudioComponent (default 1.0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_volume_multiplier"),
            TEXT("Set the volume multiplier on an existing AAmbientSound actor's AudioComponent. "
                 "Params: actor_name (string, AAmbientSound label), volume_multiplier (number, 0=silent, 1=nominal). "
                 "Workflow: call sound/spawn_ambient_actor first to create the target. "
                 "Warning: targets AAmbientSound only; use UAudioComponent::SetVolumeMultiplier directly from Python for generic actors."))
        .RequiredString(TEXT("actor_name"),        TEXT("Actor label of the AAmbientSound to adjust"))
        .RequiredNumber(TEXT("volume_multiplier"), TEXT("New linear volume multiplier (0 = silent, 1 = nominal)"))
        .Build());

    return Tools;
}

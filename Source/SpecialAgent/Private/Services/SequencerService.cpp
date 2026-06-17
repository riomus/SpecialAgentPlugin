// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/SequencerService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneSequencePlayer.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/ConstructorHelpers.h"
#include "ScopedTransaction.h"

FString FSequencerService::GetServiceDescription() const
{
    return TEXT("Level Sequence creation, bindings, tracks, keyframes, playback");
}

FMCPResponse FSequencerService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("create")) return HandleCreate(Request);
    if (MethodName == TEXT("add_actor_binding")) return HandleAddActorBinding(Request);
    if (MethodName == TEXT("add_transform_track")) return HandleAddTransformTrack(Request);
    if (MethodName == TEXT("add_keyframe")) return HandleAddKeyframe(Request);
    if (MethodName == TEXT("set_playback_range")) return HandleSetPlaybackRange(Request);
    if (MethodName == TEXT("play")) return HandlePlay(Request);

    return MethodNotFound(Request.Id, TEXT("sequencer"), MethodName);
}

TArray<FMCPToolInfo> FSequencerService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("create"),
        TEXT("Create a new ULevelSequence (cinematic) asset in the content browser. Returns {asset_path, name}; "
             "pass asset_path to every downstream sequencer tool. "
             "Params: name (string, required, bare asset name, no path or extension), "
             "package_path (string, optional, /Game/... destination folder, default /Game/Cinematics). "
             "Workflow: create -> add_actor_binding -> add_transform_track -> add_keyframe -> set_playback_range -> play. "
             "Warning: creates a new on-disk asset; FAILS if an asset of that name already exists at package_path "
             "(check first and pick a unique name rather than retrying)."))
        .RequiredString(TEXT("name"), TEXT("Bare asset name, no path or extension"))
        .OptionalString(TEXT("package_path"), TEXT("/Game/... destination folder (default /Game/Cinematics)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("add_actor_binding"),
        TEXT("Bind an existing level actor to a Level Sequence as a possessable. Returns {binding_guid, actor_name}; "
             "the binding_guid is required by add_transform_track and add_keyframe. "
             "Params: sequence_path (string, required, /Game/... object path of the ULevelSequence), "
             "actor_name (string, required, the actor's editor label, e.g. as shown in the World Outliner). "
             "Workflow: create -> add_actor_binding -> add_transform_track; keep the returned binding_guid for later calls. "
             "Warning: resolves the actor by label in the current editor world; fails if no actor with that label exists. "
             "Marks the sequence package dirty (in-memory); save the asset to persist."))
        .RequiredString(TEXT("sequence_path"), TEXT("/Game/... object path of the ULevelSequence"))
        .RequiredString(TEXT("actor_name"), TEXT("Editor label of the actor in the current editor world"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("add_transform_track"),
        TEXT("Add a 3D transform track (with a single full-range section) to an actor binding so it can hold transform keys. "
             "Returns {created (bool; false if the track already existed), binding_guid}. "
             "Params: sequence_path (string, required, /Game/... object path of the ULevelSequence), "
             "binding_guid (string, required, GUID returned by add_actor_binding). "
             "Workflow: add_actor_binding -> add_transform_track -> add_keyframe. "
             "Warning: idempotent - reuses the existing transform track if one is present, so re-calling is safe. "
             "Marks the package dirty (in-memory); save to persist."))
        .RequiredString(TEXT("sequence_path"), TEXT("/Game/... object path of the ULevelSequence"))
        .RequiredString(TEXT("binding_guid"), TEXT("GUID returned by add_actor_binding"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("add_keyframe"),
        TEXT("Add a cubic-interpolated transform keyframe on a binding's transform track at a display-rate frame. "
             "Returns {frame (the display-rate frame you passed), tick_frame (internal tick-resolution frame)}. "
             "Params: sequence_path (string, required), binding_guid (string, required), "
             "frame (integer, required, frame index in Display Rate, NOT tick resolution), "
             "location (array [X,Y,Z], optional, world cm), rotation (array [Pitch,Yaw,Roll], optional, degrees), "
             "scale (array [X,Y,Z], optional, unitless, default [1,1,1]). At least one of location/rotation/scale is required. "
             "Workflow: call add_transform_track first; set_playback_range so keys fall inside it. "
             "Warning: Sequencer transform tracks are relative to the binding. The Details panel labels the rotation "
             "channels X=Roll, Y=Pitch, Z=Yaw - this tool takes [Pitch,Yaw,Roll] and maps them correctly. "
             "Keys outside the playback range are stored but will not play. Marks the package dirty; save to persist."))
        .RequiredString(TEXT("sequence_path"), TEXT("/Game/... object path of the ULevelSequence"))
        .RequiredString(TEXT("binding_guid"), TEXT("GUID returned by add_actor_binding"))
        .RequiredInteger(TEXT("frame"), TEXT("Frame index in Display Rate (not tick resolution)"))
        .OptionalVec3(TEXT("location"), TEXT("World location [X, Y, Z] in cm"))
        .OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalVec3(TEXT("scale"), TEXT("Scale [X, Y, Z], unitless (default [1,1,1])"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_playback_range"),
        TEXT("Set the playback range of a Level Sequence, in Display-Rate frames. Returns {start_frame, end_frame}. "
             "Params: sequence_path (string, required), start_frame (integer, required, Display Rate, inclusive), "
             "end_frame (integer, required, Display Rate, exclusive). "
             "Workflow: usually called once after all keyframes are placed; play uses this range. "
             "Warning: end_frame must be strictly greater than start_frame (else rejected). Frames are Display Rate, "
             "not tick resolution. Marks the package dirty (in-memory); save the asset to persist."))
        .RequiredString(TEXT("sequence_path"), TEXT("/Game/... object path of the ULevelSequence"))
        .RequiredInteger(TEXT("start_frame"), TEXT("Start frame, Display Rate, inclusive"))
        .RequiredInteger(TEXT("end_frame"), TEXT("End frame, Display Rate, exclusive (must be > start_frame)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("play"),
        TEXT("Play a Level Sequence in the editor world. Creates a transient ULevelSequencePlayer (and backing "
             "ALevelSequenceActor) and calls Play. Returns {sequence_path, playing (bool)}. "
             "Params: sequence_path (string, required, /Game/... object path of the ULevelSequence). "
             "Workflow: finalize add_keyframe and set_playback_range before playing. "
             "Warning: plays once and does NOT loop by default; PIE is NOT started. The player is transient and not "
             "saved with the level. Editor-world actors only tick to update while the editor viewport is being redrawn."))
        .RequiredString(TEXT("sequence_path"), TEXT("/Game/... object path of the ULevelSequence"))
        .Build());

    return Tools;
}

FMCPResponse FSequencerService::HandleCreate(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString Name;
    if (!FMCPJson::ReadString(Request.Params, TEXT("name"), Name))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'name'"));
    }

    FString PackagePath = TEXT("/Game/Cinematics");
    FMCPJson::ReadString(Request.Params, TEXT("package_path"), PackagePath);

    auto Task = [Name, PackagePath]() -> TSharedPtr<FJsonObject>
    {
        // Load the factory UClass by script path. The header is in a Private folder.
        UClass* FactoryClass = LoadClass<UObject>(nullptr,
            TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"));
        if (!FactoryClass)
        {
            return FMCPJson::MakeError(TEXT("LevelSequenceFactoryNew class not found (LevelSequenceEditor loaded?)"));
        }

        UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
        if (!Factory)
        {
            return FMCPJson::MakeError(TEXT("Failed to construct LevelSequenceFactoryNew"));
        }

        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        IAssetTools& AssetTools = AssetToolsModule.Get();

        UObject* Created = AssetTools.CreateAsset(Name, PackagePath, ULevelSequence::StaticClass(), Factory);
        if (!Created)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("CreateAsset failed for %s/%s"), *PackagePath, *Name));
        }

        ULevelSequence* Sequence = Cast<ULevelSequence>(Created);
        if (!Sequence)
        {
            return FMCPJson::MakeError(TEXT("Created asset is not a ULevelSequence"));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
        Result->SetStringField(TEXT("name"), Sequence->GetName());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSequencerService::HandleAddActorBinding(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SequencePath, ActorName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sequence_path"), SequencePath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'sequence_path'"));
    }
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    }

    auto Task = [SequencePath, ActorName]() -> TSharedPtr<FJsonObject>
    {
        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!Sequence)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load sequence: %s"), *SequencePath));
        }

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

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add sequencer actor binding")));
        Sequence->Modify();

        // CreatePossessable is virtual on UMovieSceneSequence. Call via base-class pointer
        // so we don't hit ULevelSequence's protected override.
        UMovieSceneSequence* BaseSequence = Sequence;
        FGuid BindingGuid = BaseSequence->CreatePossessable(Actor);
        if (!BindingGuid.IsValid())
        {
            return FMCPJson::MakeError(TEXT("CreatePossessable returned invalid GUID"));
        }

        // Persist the binding link to the actor so tracks resolve.
        Sequence->BindPossessableObject(BindingGuid, *Actor, World);
        Sequence->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
        Result->SetStringField(TEXT("actor_name"), ActorName);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSequencerService::HandleAddTransformTrack(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SequencePath, BindingGuidString;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sequence_path"), SequencePath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'sequence_path'"));
    }
    if (!FMCPJson::ReadString(Request.Params, TEXT("binding_guid"), BindingGuidString))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'binding_guid'"));
    }

    auto Task = [SequencePath, BindingGuidString]() -> TSharedPtr<FJsonObject>
    {
        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!Sequence)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load sequence: %s"), *SequencePath));
        }

        FGuid BindingGuid;
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Invalid binding_guid: %s"), *BindingGuidString));
        }

        UMovieScene* MovieScene = Sequence->GetMovieScene();
        if (!MovieScene)
        {
            return FMCPJson::MakeError(TEXT("Sequence has no MovieScene"));
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add sequencer transform track")));
        MovieScene->Modify();

        // Idempotent — return the existing track if any.
        UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
        bool bCreated = false;
        if (!Track)
        {
            Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
            bCreated = (Track != nullptr);
        }
        if (!Track)
        {
            return FMCPJson::MakeError(TEXT("Failed to add UMovieScene3DTransformTrack"));
        }

        if (Track->GetAllSections().Num() == 0)
        {
            UMovieSceneSection* NewSection = Track->CreateNewSection();
            if (NewSection)
            {
                NewSection->SetRange(TRange<FFrameNumber>::All());
                Track->AddSection(*NewSection);
            }
        }

        Sequence->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetBoolField(TEXT("created"), bCreated);
        Result->SetStringField(TEXT("binding_guid"), BindingGuidString);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

// Channel indices for UMovieScene3DTransformSection channel proxy.
// Order from MovieScene3DTransformSection.cpp: TX TY TZ RX RY RZ SX SY SZ.
static void AddDoubleKey(UMovieScene3DTransformSection* Section, int32 Index, FFrameNumber Frame, double Value)
{
    if (!Section) return;
    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    TArrayView<FMovieSceneDoubleChannel*> Channels = Proxy.GetChannels<FMovieSceneDoubleChannel>();
    if (Index < 0 || Index >= Channels.Num()) return;
    if (FMovieSceneDoubleChannel* Ch = Channels[Index])
    {
        Ch->AddCubicKey(Frame, Value);
    }
}

FMCPResponse FSequencerService::HandleAddKeyframe(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SequencePath, BindingGuidString;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sequence_path"), SequencePath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'sequence_path'"));
    }
    if (!FMCPJson::ReadString(Request.Params, TEXT("binding_guid"), BindingGuidString))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'binding_guid'"));
    }

    int32 DisplayFrame = 0;
    if (!FMCPJson::ReadInteger(Request.Params, TEXT("frame"), DisplayFrame))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'frame'"));
    }

    FVector Location(0, 0, 0);
    FRotator Rotation(0, 0, 0);
    FVector Scale(1, 1, 1);
    const bool bHasLoc = FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);
    const bool bHasRot = FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);
    const bool bHasScale = FMCPJson::ReadVec3(Request.Params, TEXT("scale"), Scale);

    if (!bHasLoc && !bHasRot && !bHasScale)
    {
        return InvalidParams(Request.Id, TEXT("At least one of location/rotation/scale must be provided"));
    }

    auto Task = [SequencePath, BindingGuidString, DisplayFrame, Location, Rotation, Scale,
                 bHasLoc, bHasRot, bHasScale]() -> TSharedPtr<FJsonObject>
    {
        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!Sequence)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load sequence: %s"), *SequencePath));
        }

        FGuid BindingGuid;
        if (!FGuid::Parse(BindingGuidString, BindingGuid))
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Invalid binding_guid: %s"), *BindingGuidString));
        }

        UMovieScene* MovieScene = Sequence->GetMovieScene();
        if (!MovieScene)
        {
            return FMCPJson::MakeError(TEXT("Sequence has no MovieScene"));
        }

        UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
        if (!Track)
        {
            return FMCPJson::MakeError(TEXT("No transform track on binding (call sequencer/add_transform_track first)"));
        }

        // Use (or create) the first section.
        UMovieScene3DTransformSection* Section = nullptr;
        for (UMovieSceneSection* S : Track->GetAllSections())
        {
            Section = Cast<UMovieScene3DTransformSection>(S);
            if (Section) break;
        }
        if (!Section)
        {
            UMovieSceneSection* NewSection = Track->CreateNewSection();
            if (NewSection)
            {
                NewSection->SetRange(TRange<FFrameNumber>::All());
                Track->AddSection(*NewSection);
                Section = Cast<UMovieScene3DTransformSection>(NewSection);
            }
        }
        if (!Section)
        {
            return FMCPJson::MakeError(TEXT("Failed to create transform section"));
        }

        // Convert display frame to tick resolution.
        const FFrameRate DisplayRate  = MovieScene->GetDisplayRate();
        const FFrameRate TickResolution = MovieScene->GetTickResolution();
        const FFrameTime TickTime = FFrameRate::TransformTime(
            FFrameTime(FFrameNumber(DisplayFrame)), DisplayRate, TickResolution);
        const FFrameNumber TickFrame = TickTime.GetFrame();

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add sequencer keyframe")));
        Section->Modify();

        if (bHasLoc)
        {
            AddDoubleKey(Section, 0, TickFrame, Location.X);
            AddDoubleKey(Section, 1, TickFrame, Location.Y);
            AddDoubleKey(Section, 2, TickFrame, Location.Z);
        }
        if (bHasRot)
        {
            AddDoubleKey(Section, 3, TickFrame, Rotation.Roll);   // X / Roll
            AddDoubleKey(Section, 4, TickFrame, Rotation.Pitch);  // Y / Pitch
            AddDoubleKey(Section, 5, TickFrame, Rotation.Yaw);    // Z / Yaw
        }
        if (bHasScale)
        {
            AddDoubleKey(Section, 6, TickFrame, Scale.X);
            AddDoubleKey(Section, 7, TickFrame, Scale.Y);
            AddDoubleKey(Section, 8, TickFrame, Scale.Z);
        }

        Sequence->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetNumberField(TEXT("frame"), DisplayFrame);
        Result->SetNumberField(TEXT("tick_frame"), TickFrame.Value);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSequencerService::HandleSetPlaybackRange(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SequencePath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sequence_path"), SequencePath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'sequence_path'"));
    }

    int32 StartDisplay = 0, EndDisplay = 0;
    if (!FMCPJson::ReadInteger(Request.Params, TEXT("start_frame"), StartDisplay))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'start_frame'"));
    }
    if (!FMCPJson::ReadInteger(Request.Params, TEXT("end_frame"), EndDisplay))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'end_frame'"));
    }
    if (EndDisplay <= StartDisplay)
    {
        return InvalidParams(Request.Id, TEXT("'end_frame' must be greater than 'start_frame'"));
    }

    auto Task = [SequencePath, StartDisplay, EndDisplay]() -> TSharedPtr<FJsonObject>
    {
        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!Sequence)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load sequence: %s"), *SequencePath));
        }

        UMovieScene* MovieScene = Sequence->GetMovieScene();
        if (!MovieScene)
        {
            return FMCPJson::MakeError(TEXT("Sequence has no MovieScene"));
        }

        const FFrameRate DisplayRate    = MovieScene->GetDisplayRate();
        const FFrameRate TickResolution = MovieScene->GetTickResolution();
        const FFrameNumber StartTick = FFrameRate::TransformTime(
            FFrameTime(FFrameNumber(StartDisplay)), DisplayRate, TickResolution).GetFrame();
        const FFrameNumber EndTick = FFrameRate::TransformTime(
            FFrameTime(FFrameNumber(EndDisplay)), DisplayRate, TickResolution).GetFrame();

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set sequencer playback range")));
        MovieScene->Modify();
        MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartTick, EndTick));
        Sequence->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetNumberField(TEXT("start_frame"), StartDisplay);
        Result->SetNumberField(TEXT("end_frame"), EndDisplay);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSequencerService::HandlePlay(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SequencePath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("sequence_path"), SequencePath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'sequence_path'"));
    }

    auto Task = [SequencePath]() -> TSharedPtr<FJsonObject>
    {
        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!Sequence)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load sequence: %s"), *SequencePath));
        }

        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        ALevelSequenceActor* OutActor = nullptr;
        FMovieSceneSequencePlaybackSettings Settings;
        ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, Settings, OutActor);
        if (!Player)
        {
            return FMCPJson::MakeError(TEXT("CreateLevelSequencePlayer returned null"));
        }

        Player->Play();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("sequence_path"), SequencePath);
        Result->SetBoolField(TEXT("playing"), Player->IsPlaying());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

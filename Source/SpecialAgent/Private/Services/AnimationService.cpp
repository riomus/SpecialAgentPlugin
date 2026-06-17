// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/AnimationService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
    USkeletalMeshComponent* ResolveSkelMesh(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor) return nullptr;
        if (!ComponentName.IsEmpty())
        {
            for (UActorComponent* Comp : Actor->GetComponents())
            {
                if (Comp && Comp->GetName() == ComponentName)
                {
                    return Cast<USkeletalMeshComponent>(Comp);
                }
            }
            return nullptr;
        }
        for (UActorComponent* Comp : Actor->GetComponents())
        {
            if (USkeletalMeshComponent* Skel = Cast<USkeletalMeshComponent>(Comp))
            {
                return Skel;
            }
        }
        return nullptr;
    }
}

FString FAnimationService::GetServiceDescription() const
{
    return TEXT("Skeletal mesh animation playback and anim blueprint assignment");
}

FMCPResponse FAnimationService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("play"))               return HandlePlay(Request);
    if (MethodName == TEXT("stop"))               return HandleStop(Request);
    if (MethodName == TEXT("set_anim_blueprint")) return HandleSetAnimBlueprint(Request);
    if (MethodName == TEXT("list_animations"))    return HandleListAnimations(Request);
    if (MethodName == TEXT("set_pose"))           return HandleSetPose(Request);

    return MethodNotFound(Request.Id, TEXT("animation"), MethodName);
}

FMCPResponse FAnimationService::HandlePlay(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName, AnimPath;
    bool bLooping = true;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("animation_path"), AnimPath))
        return InvalidParams(Request.Id, TEXT("Missing 'animation_path' (asset path to UAnimSequence)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);
    FMCPJson::ReadBool  (Request.Params, TEXT("looping"),        bLooping);

    auto Task = [ActorName, ComponentName, AnimPath, bLooping]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        USkeletalMeshComponent* Skel = ResolveSkelMesh(Actor, ComponentName);
        if (!Skel) return FMCPJson::MakeError(TEXT("No USkeletalMeshComponent on actor"));

        UAnimationAsset* Asset = LoadObject<UAnimationAsset>(nullptr, *AnimPath);
        if (!Asset) return FMCPJson::MakeError(FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath));

        Skel->PlayAnimation(Asset, bLooping);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"),     Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),      Skel->GetName());
        Result->SetStringField(TEXT("animation_path"), AnimPath);
        Result->SetBoolField  (TEXT("looping"),        bLooping);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: animation/play %s.%s -> %s (loop=%d)"),
            *Actor->GetActorLabel(), *Skel->GetName(), *AnimPath, bLooping ? 1 : 0);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAnimationService::HandleStop(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    auto Task = [ActorName, ComponentName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        USkeletalMeshComponent* Skel = ResolveSkelMesh(Actor, ComponentName);
        if (!Skel) return FMCPJson::MakeError(TEXT("No USkeletalMeshComponent on actor"));

        Skel->Stop();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Skel->GetName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: animation/stop %s.%s"), *Actor->GetActorLabel(), *Skel->GetName());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAnimationService::HandleSetAnimBlueprint(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName, AnimBPPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("anim_blueprint_path"), AnimBPPath))
        return InvalidParams(Request.Id, TEXT("Missing 'anim_blueprint_path' (asset path to UAnimBlueprint)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    auto Task = [ActorName, ComponentName, AnimBPPath]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        USkeletalMeshComponent* Skel = ResolveSkelMesh(Actor, ComponentName);
        if (!Skel) return FMCPJson::MakeError(TEXT("No USkeletalMeshComponent on actor"));

        UClass* GenClass = nullptr;
        if (AnimBPPath.EndsWith(TEXT("_C")))
        {
            GenClass = LoadObject<UClass>(nullptr, *AnimBPPath);
        }
        if (!GenClass)
        {
            if (UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath))
            {
                GenClass = AnimBP->GeneratedClass;
            }
        }
        if (!GenClass)
        {
            const FString WithC = AnimBPPath + TEXT("_C");
            GenClass = LoadObject<UClass>(nullptr, *WithC);
        }
        if (!GenClass) return FMCPJson::MakeError(FString::Printf(TEXT("Anim blueprint not found: %s"), *AnimBPPath));

        if (!GenClass->IsChildOf(UAnimInstance::StaticClass()))
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Class is not a UAnimInstance subclass: %s"), *GenClass->GetName()));
        }

        Skel->SetAnimInstanceClass(GenClass);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"),          Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),           Skel->GetName());
        Result->SetStringField(TEXT("anim_blueprint_path"), AnimBPPath);
        Result->SetStringField(TEXT("anim_instance_class"), GenClass->GetName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: animation/set_anim_blueprint %s.%s -> %s"),
            *Actor->GetActorLabel(), *Skel->GetName(), *GenClass->GetName());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAnimationService::HandleListAnimations(const FMCPRequest& Request)
{
    FString PathFilter;
    int32 MaxResults = 1000;
    if (Request.Params.IsValid())
    {
        FMCPJson::ReadString(Request.Params, TEXT("path"), PathFilter);
        FMCPJson::ReadInteger(Request.Params, TEXT("max_results"), MaxResults);
    }

    auto Task = [PathFilter, MaxResults]() -> TSharedPtr<FJsonObject>
    {
        IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        if (!PathFilter.IsEmpty())
        {
            Filter.PackagePaths.Add(FName(*PathFilter));
            Filter.bRecursivePaths = true;
        }

        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);
        if (MaxResults >= 0 && Assets.Num() > MaxResults) Assets.SetNum(MaxResults);

        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Assets.Num());
        for (const FAssetData& A : Assets)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"),  A.AssetName.ToString());
            Obj->SetStringField(TEXT("path"),  A.GetObjectPathString());
            Obj->SetStringField(TEXT("class"), A.AssetClassPath.ToString());
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetArrayField(TEXT("animations"), Out);
        Result->SetNumberField(TEXT("count"), Out.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: animation/list_animations -> %d entries (path=%s)"),
            Out.Num(), PathFilter.IsEmpty() ? TEXT("<all>") : *PathFilter);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAnimationService::HandleSetPose(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName, AnimPath;
    double Time = 0.0;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("animation_path"), AnimPath))
        return InvalidParams(Request.Id, TEXT("Missing 'animation_path'"));
    if (!FMCPJson::ReadNumber(Request.Params, TEXT("time"), Time))
        return InvalidParams(Request.Id, TEXT("Missing 'time' (seconds into the animation)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    auto Task = [ActorName, ComponentName, AnimPath, Time]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        USkeletalMeshComponent* Skel = ResolveSkelMesh(Actor, ComponentName);
        if (!Skel) return FMCPJson::MakeError(TEXT("No USkeletalMeshComponent on actor"));

        UAnimationAsset* Asset = LoadObject<UAnimationAsset>(nullptr, *AnimPath);
        if (!Asset) return FMCPJson::MakeError(FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath));

        Skel->PlayAnimation(Asset, /*bLooping*/ false);
        Skel->SetPosition(static_cast<float>(Time));
        Skel->SetPlayRate(0.0f);
        Skel->bPauseAnims = true;

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"),     Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),      Skel->GetName());
        Result->SetStringField(TEXT("animation_path"), AnimPath);
        Result->SetNumberField(TEXT("time"),           Time);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: animation/set_pose %s.%s -> %s @ %.3fs"),
            *Actor->GetActorLabel(), *Skel->GetName(), *AnimPath, Time);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FAnimationService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("play"),
            TEXT("Play an animation asset on a skeletal mesh component via PlayAnimation (puts the component in single-node mode). "
                 "Returns {actor_name, component, animation_path, looping}. "
                 "Params: actor_name (string, required, actor editor label), "
                 "animation_path (string, required, /Game/... object path to a UAnimationAsset - usually a UAnimSequence, "
                 "e.g. /Game/Anims/Run.Run), looping (bool, optional, default true), "
                 "component_name (string, optional; targets a specific USkeletalMeshComponent, else the first one found). "
                 "Workflow: use list_animations to discover asset paths. "
                 "Warning: single-node mode and set_anim_blueprint are mutually exclusive - this clears any assigned AnimBlueprint "
                 "until you call set_anim_blueprint again. Plays in PIE, but editor preview does NOT tick by default (the mesh "
                 "freezes on one frame) - enable update-animation-in-editor on the component to see motion in the editor. "
                 "State is transient (not serialized)."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor editor label"))
        .RequiredString(TEXT("animation_path"), TEXT("/Game/... object path to a UAnimationAsset (usually a UAnimSequence)"))
        .OptionalBool  (TEXT("looping"),        TEXT("Loop the animation (default true)"))
        .OptionalString(TEXT("component_name"), TEXT("Specific USkeletalMeshComponent; defaults to first found"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("stop"),
            TEXT("Stop the single-node animation currently playing on a skeletal mesh component. Returns {actor_name, component}. "
                 "Params: actor_name (string, required, actor editor label), "
                 "component_name (string, optional; targets a specific USkeletalMeshComponent, else the first one found). "
                 "Workflow: inverse of the play tool; the component stays in single-node mode (it does not restore an AnimBlueprint). "
                 "Warning: no-op if nothing is playing; to drive the mesh again use play or set_anim_blueprint."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor editor label"))
        .OptionalString(TEXT("component_name"), TEXT("Specific USkeletalMeshComponent; defaults to first found"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_anim_blueprint"),
            TEXT("Assign an AnimBlueprint (via its generated UAnimInstance class) to a skeletal mesh component using "
                 "SetAnimInstanceClass. Returns {actor_name, component, anim_blueprint_path, anim_instance_class}. "
                 "Params: actor_name (string, required, actor editor label), "
                 "anim_blueprint_path (string, required, /Game/... object path to a UAnimBlueprint; a trailing _C generated-class "
                 "suffix is optional and resolved automatically), "
                 "component_name (string, optional; targets a specific USkeletalMeshComponent, else the first one found). "
                 "Workflow: sets up gameplay-driven animation; the AnimBlueprint then runs from its variables/state machine. "
                 "Warning: mutually exclusive with the play tool's single-node mode - assigning a blueprint clears any single "
                 "animation being played. Errors if the resolved class is not a UAnimInstance subclass."))
        .RequiredString(TEXT("actor_name"),          TEXT("Actor editor label"))
        .RequiredString(TEXT("anim_blueprint_path"), TEXT("/Game/... object path to a UAnimBlueprint (trailing _C optional)"))
        .OptionalString(TEXT("component_name"),      TEXT("Specific USkeletalMeshComponent; defaults to first found"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("list_animations"),
            TEXT("Enumerate UAnimSequence assets (including subclasses) from the asset registry without loading them. "
                 "Returns {animations: [{name, path (object path), class}], count}. "
                 "Params: path (string, optional, package-path folder to search, e.g. /Game/Anims; searched recursively; "
                 "omit to scan all paths), max_results (integer, optional, default 1000; a negative value returns all matches uncapped). "
                 "Workflow: feed an entry's path into the play or set_pose tool's animation_path. "
                 "Warning: read-only registry query (no assets are loaded or modified); results reflect cached metadata."))
        .OptionalString (TEXT("path"),        TEXT("Package-path folder to restrict the search (recursive); omit for all paths"))
        .OptionalInteger(TEXT("max_results"), TEXT("Cap on returned entries (default 1000; negative = uncapped)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_pose"),
            TEXT("Freeze a skeletal mesh on a single pose of an animation: plays the asset paused (play rate 0) at the requested time. "
                 "Returns {actor_name, component, animation_path, time}. "
                 "Params: actor_name (string, required, actor editor label), "
                 "animation_path (string, required, /Game/... object path to a UAnimationAsset), "
                 "time (number, required, seconds into the animation; clamped to [0, sequence length]), "
                 "component_name (string, optional; targets a specific USkeletalMeshComponent, else the first one found). "
                 "Workflow: useful for staging a deterministic pose before capturing a screenshot. "
                 "Warning: enters single-node mode (clears any AnimBlueprint, like play); times past the sequence length resolve to the "
                 "last frame. Editor preview does not tick by default, so enable update-animation-in-editor on the component to see the pose."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor editor label"))
        .RequiredString(TEXT("animation_path"), TEXT("/Game/... object path to a UAnimationAsset"))
        .RequiredNumber(TEXT("time"),           TEXT("Time in seconds into the animation (clamped to [0, length])"))
        .OptionalString(TEXT("component_name"), TEXT("Specific USkeletalMeshComponent; defaults to first found"))
        .Build());

    return Tools;
}

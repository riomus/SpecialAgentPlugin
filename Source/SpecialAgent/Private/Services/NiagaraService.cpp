// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/NiagaraService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FString FNiagaraService::GetServiceDescription() const
{
    return TEXT("Niagara VFX spawning and parameter control");
}

FMCPResponse FNiagaraService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("spawn_emitter"))   return HandleSpawnEmitter(Request);
    if (MethodName == TEXT("set_parameter"))   return HandleSetParameter(Request);
    if (MethodName == TEXT("activate"))        return HandleActivate(Request);
    if (MethodName == TEXT("deactivate"))      return HandleDeactivate(Request);
    if (MethodName == TEXT("set_user_float"))  return HandleSetUserFloat(Request);
    if (MethodName == TEXT("set_user_vec3"))   return HandleSetUserVec3(Request);

    return MethodNotFound(Request.Id, TEXT("niagara"), MethodName);
}

TArray<FMCPToolInfo> FNiagaraService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("spawn_emitter"),
        TEXT("Spawn a Niagara system at a world location and auto-activate it. Returns {actor_name (label, use for all "
             "follow-up calls), component_name, system_path}. "
             "Params: system_path (string, required, /Game/... object path of a UNiagaraSystem asset), "
             "location (array [X,Y,Z], required, world cm), rotation (array [Pitch,Yaw,Roll], optional, degrees), "
             "auto_destroy (bool, optional, default true; destroys the actor once the system finishes). "
             "Workflow: spawn_emitter -> set_user_float / set_user_vec3 / set_parameter to configure. "
             "Warning: system_path MUST be a UNiagaraSystem, not a UNiagaraEmitter or script (the #1 mistake). "
             "Spawns auto-activated, so User. params set after this take effect next activate(reset=true), not retroactively. "
             "Looping systems with auto_destroy=false linger until you deactivate and remove the actor."))
        .RequiredString(TEXT("system_path"), TEXT("/Game/... object path of a UNiagaraSystem"))
        .RequiredVec3(TEXT("location"), TEXT("Spawn location [X, Y, Z] in world cm"))
        .OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalBool(TEXT("auto_destroy"), TEXT("Destroy the actor when the system completes (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_parameter"),
        TEXT("Set a float parameter on a spawned Niagara component via SetFloatParameter. Returns {actor_name, parameter, value}. "
             "For exposed User. namespace variables prefer set_user_float instead. "
             "Params: actor_name (string, required, actor label from spawn_emitter), "
             "parameter (string, required, the float parameter name), value (number, required). "
             "Workflow: spawn_emitter -> set_parameter. "
             "Warning: STRONGLY TYPED to float - the parameter must be a declared float on the system. "
             "The name is verified against the component override parameters first; an unexposed or "
             "wrong-typed name now returns an error instead of silently succeeding."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label of the Niagara actor from spawn_emitter"))
        .RequiredString(TEXT("parameter"), TEXT("Name of the float parameter to set"))
        .RequiredNumber(TEXT("value"), TEXT("Float value to assign"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("activate"),
        TEXT("Activate a spawned Niagara component to (re)start emission. Returns {actor_name, is_active (bool)}. "
             "Params: actor_name (string, required, actor label from spawn_emitter), "
             "reset (bool, optional, default false; true restarts the system from its initial state). "
             "Workflow: spawn_emitter auto-activates, so use this mainly after deactivate or to reset; "
             "set User. params (set_user_float / set_user_vec3) before activate(reset=true) so they take effect. "
             "Warning: activating an already-active component is a no-op unless reset=true."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label of the Niagara actor from spawn_emitter"))
        .OptionalBool(TEXT("reset"), TEXT("Restart the system from its initial state (default false)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("deactivate"),
        TEXT("Deactivate a spawned Niagara component: stops emission, existing particles finish their lifetime and die off. "
             "Returns {actor_name, is_active (bool)}. "
             "Params: actor_name (string, required, actor label from spawn_emitter). "
             "Workflow: pair with activate to toggle a system on and off. "
             "Warning: does NOT destroy the actor or component (use the world delete-actor tool to remove it); "
             "re-enable later with activate."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label of the Niagara actor from spawn_emitter"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_user_float"),
        TEXT("Set a User. namespace float exposure variable on a spawned Niagara component via SetVariableFloat. "
             "Returns {actor_name, name, value}. Give the bare name without the 'User.' prefix (it is added internally). "
             "Params: actor_name (string, required, actor label from spawn_emitter), "
             "name (string, required, variable name WITHOUT the 'User.' prefix), value (number, required). "
             "Workflow: spawn_emitter -> set_user_float; set before activate(reset=true) so it applies on this run. "
             "Warning: only affects User. exposure variables that are declared as float and exposed by the system. "
             "The name is verified against the component override parameters first, so an unexposed or "
             "wrong-typed name now returns an error instead of a silent no-op. For non-User parameters use set_parameter."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label of the Niagara actor from spawn_emitter"))
        .RequiredString(TEXT("name"), TEXT("User variable name, WITHOUT the 'User.' prefix"))
        .RequiredNumber(TEXT("value"), TEXT("Float value to assign"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_user_vec3"),
        TEXT("Set a User. namespace Vector (vec3) exposure variable on a spawned Niagara component via SetVariableVec3. "
             "Returns {actor_name, name, value [X,Y,Z]}. Give the bare name without the 'User.' prefix (added internally). "
             "Params: actor_name (string, required, actor label from spawn_emitter), "
             "name (string, required, variable name WITHOUT the 'User.' prefix), "
             "value (array [X,Y,Z], required; interpret as cm only if the variable is a position). "
             "Workflow: spawn_emitter -> set_user_vec3; set before activate(reset=true) so it applies on this run. "
             "Warning: only affects User. exposure variables declared as Vector (or Position) and exposed by the system. "
             "The name is verified against the component override parameters first, so an unexposed or "
             "wrong-typed name now returns an error instead of a silent no-op."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label of the Niagara actor from spawn_emitter"))
        .RequiredString(TEXT("name"), TEXT("User variable name, WITHOUT the 'User.' prefix"))
        .RequiredVec3(TEXT("value"), TEXT("Vector value [X, Y, Z]"))
        .Build());

    return Tools;
}

namespace
{
    // Resolve a Niagara component from an actor label. Accepts either:
    //   - A spawned ANiagaraActor (uses its NiagaraComponent).
    //   - Any actor that happens to own a UNiagaraComponent (first one found).
    UNiagaraComponent* ResolveNiagaraComponent(UWorld* World, const FString& ActorName, FString& OutError)
    {
        if (!World)
        {
            OutError = TEXT("No editor world");
            return nullptr;
        }
        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor)
        {
            OutError = FString::Printf(TEXT("Actor not found: %s"), *ActorName);
            return nullptr;
        }

        if (ANiagaraActor* NActor = Cast<ANiagaraActor>(Actor))
        {
            if (UNiagaraComponent* Comp = NActor->GetNiagaraComponent())
            {
                return Comp;
            }
        }

        UNiagaraComponent* Comp = Actor->FindComponentByClass<UNiagaraComponent>();
        if (!Comp)
        {
            OutError = FString::Printf(TEXT("Actor '%s' has no UNiagaraComponent"), *ActorName);
            return nullptr;
        }
        return Comp;
    }

    // Verify that a parameter of the given type is actually declared on the component's
    // override parameter store before we try to set it. The override store is a
    // FNiagaraUserRedirectionParameterStore, so a bare (User.-less) name is redirected to its
    // fully-qualified User.* variable, and FindParameterOffset(.., IgnoreType=false) matches on
    // both name AND type. A null offset means the parameter is unexposed/undeclared OR the type
    // does not match - in either case the engine's SetVariable* would silently add a stray entry
    // instead of reporting failure, so we treat it as an error here.
    bool NiagaraParamExists(UNiagaraComponent* Comp, const FName& VarName, const FNiagaraTypeDefinition& Type)
    {
        if (!Comp)
        {
            return false;
        }
        const FNiagaraParameterStore& Store = Comp->GetOverrideParameters();
        const FNiagaraVariableBase Var(Type, VarName);
        return Store.FindParameterOffset(Var, /*IgnoreType*/false) != nullptr;
    }
}

FMCPResponse FNiagaraService::HandleSpawnEmitter(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString SystemPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("system_path"), SystemPath))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'system_path'"));
    }

    FVector Location;
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
    {
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'location'"));
    }

    FRotator Rotation(0, 0, 0);
    FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

    bool bAutoDestroy = true;
    FMCPJson::ReadBool(Request.Params, TEXT("auto_destroy"), bAutoDestroy);

    auto Task = [SystemPath, Location, Rotation, bAutoDestroy]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
        if (!System)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load UNiagaraSystem: %s"), *SystemPath));
        }

        UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
            World, System, Location, Rotation, FVector(1.f), bAutoDestroy, /*bAutoActivate*/true);
        if (!Comp)
        {
            return FMCPJson::MakeError(TEXT("SpawnSystemAtLocation returned null"));
        }

        AActor* Owner = Comp->GetOwner();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Owner ? Owner->GetActorLabel() : TEXT(""));
        Result->SetStringField(TEXT("component_name"), Comp->GetName());
        Result->SetStringField(TEXT("system_path"), SystemPath);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNiagaraService::HandleSetParameter(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString ActorName, ParameterName;
    double Value = 0.0;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    }
    if (!FMCPJson::ReadString(Request.Params, TEXT("parameter"), ParameterName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'parameter'"));
    }
    if (!FMCPJson::ReadNumber(Request.Params, TEXT("value"), Value))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'value'"));
    }

    auto Task = [ActorName, ParameterName, Value]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        FString Err;
        UNiagaraComponent* Comp = ResolveNiagaraComponent(World, ActorName, Err);
        if (!Comp)
        {
            return FMCPJson::MakeError(Err);
        }

        const FName VarFName(*ParameterName);
        if (!NiagaraParamExists(Comp, VarFName, FNiagaraTypeDefinition::GetFloatDef()))
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Niagara float parameter '%s' is not exposed/declared as a float on actor '%s'. "
                     "SetFloatParameter would silently no-op. Check the parameter name and type "
                     "(User. namespace params should usually be set via set_user_float)."),
                *ParameterName, *ActorName));
        }

        Comp->SetFloatParameter(VarFName, static_cast<float>(Value));

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetStringField(TEXT("parameter"), ParameterName);
        Result->SetNumberField(TEXT("value"), Value);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNiagaraService::HandleActivate(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString ActorName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    }

    bool bReset = false;
    FMCPJson::ReadBool(Request.Params, TEXT("reset"), bReset);

    auto Task = [ActorName, bReset]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        FString Err;
        UNiagaraComponent* Comp = ResolveNiagaraComponent(World, ActorName, Err);
        if (!Comp)
        {
            return FMCPJson::MakeError(Err);
        }

        Comp->Activate(bReset);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetBoolField(TEXT("is_active"), Comp->IsActive());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNiagaraService::HandleDeactivate(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString ActorName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    }

    auto Task = [ActorName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        FString Err;
        UNiagaraComponent* Comp = ResolveNiagaraComponent(World, ActorName, Err);
        if (!Comp)
        {
            return FMCPJson::MakeError(Err);
        }

        Comp->Deactivate();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetBoolField(TEXT("is_active"), Comp->IsActive());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNiagaraService::HandleSetUserFloat(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString ActorName, VarName;
    double Value = 0.0;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    }
    if (!FMCPJson::ReadString(Request.Params, TEXT("name"), VarName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'name'"));
    }
    if (!FMCPJson::ReadNumber(Request.Params, TEXT("value"), Value))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'value'"));
    }

    auto Task = [ActorName, VarName, Value]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        FString Err;
        UNiagaraComponent* Comp = ResolveNiagaraComponent(World, ActorName, Err);
        if (!Comp)
        {
            return FMCPJson::MakeError(Err);
        }

        const FName VarFName(*VarName);
        if (!NiagaraParamExists(Comp, VarFName, FNiagaraTypeDefinition::GetFloatDef()))
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("User float parameter 'User.%s' is not exposed as a float on actor '%s'. "
                     "SetVariableFloat would silently no-op. Give the bare name (no 'User.' prefix) "
                     "and confirm the system exposes it as a float user parameter."),
                *VarName, *ActorName));
        }

        Comp->SetVariableFloat(VarFName, static_cast<float>(Value));

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetStringField(TEXT("name"), VarName);
        Result->SetNumberField(TEXT("value"), Value);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FNiagaraService::HandleSetUserVec3(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString ActorName, VarName;
    FVector Value;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    }
    if (!FMCPJson::ReadString(Request.Params, TEXT("name"), VarName))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'name'"));
    }
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("value"), Value))
    {
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'value'"));
    }

    auto Task = [ActorName, VarName, Value]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        FString Err;
        UNiagaraComponent* Comp = ResolveNiagaraComponent(World, ActorName, Err);
        if (!Comp)
        {
            return FMCPJson::MakeError(Err);
        }

        // SetVariableVec3 also accepts a Position-typed parameter (it forwards to
        // SetVariablePosition), so accept either Vector or Position to avoid false negatives.
        const FName VarFName(*VarName);
        if (!NiagaraParamExists(Comp, VarFName, FNiagaraTypeDefinition::GetVec3Def())
            && !NiagaraParamExists(Comp, VarFName, FNiagaraTypeDefinition::GetPositionDef()))
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("User vector parameter 'User.%s' is not exposed as a Vector/Position on actor '%s'. "
                     "SetVariableVec3 would silently no-op. Give the bare name (no 'User.' prefix) "
                     "and confirm the system exposes it as a Vector (or Position) user parameter."),
                *VarName, *ActorName));
        }

        Comp->SetVariableVec3(VarFName, Value);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetStringField(TEXT("name"), VarName);
        FMCPJson::WriteVec3(Result, TEXT("value"), Value);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

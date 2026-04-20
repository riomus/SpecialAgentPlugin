// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/NiagaraService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FString FNiagaraService::GetServiceDescription() const
{
    return TEXT("Niagara VFX spawning and parameter control");
}

FMCPResponse FNiagaraService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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
        TEXT("Spawn a Niagara system in the world at a location. Returns the spawned actor label. "
             "Params: system_path (string, /Game/... UNiagaraSystem asset), location ([X,Y,Z] cm), "
             "rotation ([Pitch,Yaw,Roll] deg, optional), auto_destroy (bool, optional). "
             "Workflow: use set_user_float / set_user_vec3 to configure after spawn. "
             "Warning: asset must be a UNiagaraSystem, not an emitter/script."))
        .RequiredString(TEXT("system_path"), TEXT("Asset path to UNiagaraSystem"))
        .RequiredVec3(TEXT("location"), TEXT("Spawn location [X, Y, Z] cm"))
        .OptionalVec3(TEXT("rotation"), TEXT("Rotation [Pitch, Yaw, Roll] deg"))
        .OptionalBool(TEXT("auto_destroy"), TEXT("Destroy when system completes (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_parameter"),
        TEXT("Set a float parameter on a spawned Niagara actor's component. "
             "Params: actor_name (string, label), parameter (string, variable name), value (number). "
             "Workflow: spawn_emitter -> set_parameter. "
             "Warning: parameter must match a float variable exposed by the system."))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the Niagara actor in the world"))
        .RequiredString(TEXT("parameter"), TEXT("Niagara float parameter name"))
        .RequiredNumber(TEXT("value"), TEXT("Float value"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("activate"),
        TEXT("Activate a spawned Niagara system (start emission). "
             "Params: actor_name (string, label), reset (bool, optional - true to reset state). "
             "Workflow: spawn_emitter -> activate (if spawned inactive). "
             "Warning: no-op on an already active component."))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the Niagara actor in the world"))
        .OptionalBool(TEXT("reset"), TEXT("Reset system state on activation (default false)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("deactivate"),
        TEXT("Deactivate a spawned Niagara system (stop emission, particles die off). "
             "Params: actor_name (string, label). "
             "Workflow: activate -> deactivate pairs for toggle. "
             "Warning: does not destroy the actor; call world/delete_actor to remove."))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the Niagara actor in the world"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_user_float"),
        TEXT("Set a 'User.' float variable on a spawned Niagara actor (without the prefix). "
             "Params: actor_name (string, label), name (string, variable name w/o 'User.'), value (number). "
             "Workflow: spawn_emitter -> set_user_float. "
             "Warning: affects only 'User.' namespace; system parameters use set_parameter."))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the Niagara actor in the world"))
        .RequiredString(TEXT("name"), TEXT("User variable name (without 'User.' prefix)"))
        .RequiredNumber(TEXT("value"), TEXT("Float value"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_user_vec3"),
        TEXT("Set a 'User.' vec3 variable on a spawned Niagara actor (without the prefix). "
             "Params: actor_name (string, label), name (string, variable name w/o 'User.'), value ([X,Y,Z]). "
             "Workflow: spawn_emitter -> set_user_vec3. "
             "Warning: affects only 'User.' namespace; exposed variables only."))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the Niagara actor in the world"))
        .RequiredString(TEXT("name"), TEXT("User variable name (without 'User.' prefix)"))
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

        Comp->SetFloatParameter(FName(*ParameterName), static_cast<float>(Value));

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

        Comp->SetVariableFloat(FName(*VarName), static_cast<float>(Value));

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

        Comp->SetVariableVec3(FName(*VarName), Value);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetStringField(TEXT("name"), VarName);
        FMCPJson::WriteVec3(Result, TEXT("value"), Value);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

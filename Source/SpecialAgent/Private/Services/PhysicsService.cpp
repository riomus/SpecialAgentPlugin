// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PhysicsService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/EngineTypes.h"

namespace
{
    // Resolve the primitive component to act on. If 'component_name' is provided,
    // find it by name; otherwise pick the root primitive component.
    UPrimitiveComponent* ResolvePrimComp(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor) return nullptr;
        if (!ComponentName.IsEmpty())
        {
            for (UActorComponent* Comp : Actor->GetComponents())
            {
                if (Comp && Comp->GetName() == ComponentName)
                {
                    return Cast<UPrimitiveComponent>(Comp);
                }
            }
            return nullptr;
        }
        // Prefer root primitive, else first found
        if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Actor->GetRootComponent()))
        {
            return Root;
        }
        for (UActorComponent* Comp : Actor->GetComponents())
        {
            if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
            {
                return Prim;
            }
        }
        return nullptr;
    }

    ECollisionEnabled::Type ParseCollisionEnabled(const FString& Str, bool& bOk)
    {
        bOk = true;
        if (Str.Equals(TEXT("NoCollision"),      ESearchCase::IgnoreCase)) return ECollisionEnabled::NoCollision;
        if (Str.Equals(TEXT("QueryOnly"),        ESearchCase::IgnoreCase)) return ECollisionEnabled::QueryOnly;
        if (Str.Equals(TEXT("PhysicsOnly"),      ESearchCase::IgnoreCase)) return ECollisionEnabled::PhysicsOnly;
        if (Str.Equals(TEXT("QueryAndPhysics"),  ESearchCase::IgnoreCase)) return ECollisionEnabled::QueryAndPhysics;
        bOk = false;
        return ECollisionEnabled::NoCollision;
    }
}

FString FPhysicsService::GetServiceDescription() const
{
    return TEXT("Physics simulation and body property control");
}

FMCPResponse FPhysicsService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    if (MethodName == TEXT("set_simulate_physics"))  return HandleSetSimulatePhysics(Request);
    if (MethodName == TEXT("apply_impulse"))         return HandleApplyImpulse(Request);
    if (MethodName == TEXT("apply_force"))           return HandleApplyForce(Request);
    if (MethodName == TEXT("set_linear_velocity"))   return HandleSetLinearVelocity(Request);
    if (MethodName == TEXT("set_angular_velocity"))  return HandleSetAngularVelocity(Request);
    if (MethodName == TEXT("set_mass"))              return HandleSetMass(Request);
    if (MethodName == TEXT("set_collision_enabled")) return HandleSetCollisionEnabled(Request);

    return MethodNotFound(Request.Id, TEXT("physics"), MethodName);
}

FMCPResponse FPhysicsService::HandleSetSimulatePhysics(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    bool bSimulate = false;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadBool(Request.Params, TEXT("simulate"), bSimulate))
        return InvalidParams(Request.Id, TEXT("Missing 'simulate' (bool)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    auto Task = [ActorName, ComponentName, bSimulate]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->SetSimulatePhysics(bSimulate);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        Result->SetBoolField  (TEXT("simulate"),   bSimulate);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/set_simulate_physics %s.%s=%d"),
            *Actor->GetActorLabel(), *Prim->GetName(), bSimulate ? 1 : 0);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPhysicsService::HandleApplyImpulse(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    FVector Impulse;
    bool bVelChange = false;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("impulse"), Impulse))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'impulse' [X,Y,Z]"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);
    FMCPJson::ReadBool(Request.Params, TEXT("velocity_change"), bVelChange);

    auto Task = [ActorName, ComponentName, Impulse, bVelChange]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->AddImpulse(Impulse, NAME_None, bVelChange);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        FMCPJson::WriteVec3(Result, TEXT("impulse"), Impulse);
        Result->SetBoolField(TEXT("velocity_change"), bVelChange);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/apply_impulse %s.%s (%.1f,%.1f,%.1f)"),
            *Actor->GetActorLabel(), *Prim->GetName(), Impulse.X, Impulse.Y, Impulse.Z);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPhysicsService::HandleApplyForce(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    FVector Force;
    bool bAccelChange = false;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("force"), Force))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'force' [X,Y,Z]"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);
    FMCPJson::ReadBool(Request.Params, TEXT("accel_change"), bAccelChange);

    auto Task = [ActorName, ComponentName, Force, bAccelChange]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->AddForce(Force, NAME_None, bAccelChange);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        FMCPJson::WriteVec3(Result, TEXT("force"), Force);
        Result->SetBoolField(TEXT("accel_change"), bAccelChange);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/apply_force %s.%s (%.1f,%.1f,%.1f)"),
            *Actor->GetActorLabel(), *Prim->GetName(), Force.X, Force.Y, Force.Z);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPhysicsService::HandleSetLinearVelocity(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    FVector Velocity;
    bool bAdd = false;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("velocity"), Velocity))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'velocity' [X,Y,Z] (cm/s)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);
    FMCPJson::ReadBool(Request.Params, TEXT("add_to_current"), bAdd);

    auto Task = [ActorName, ComponentName, Velocity, bAdd]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->SetPhysicsLinearVelocity(Velocity, bAdd);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        FMCPJson::WriteVec3(Result, TEXT("velocity"), Velocity);
        Result->SetBoolField(TEXT("add_to_current"), bAdd);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/set_linear_velocity %s.%s (%.1f,%.1f,%.1f)"),
            *Actor->GetActorLabel(), *Prim->GetName(), Velocity.X, Velocity.Y, Velocity.Z);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPhysicsService::HandleSetAngularVelocity(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    FVector Velocity;
    bool bAdd = false;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("angular_velocity"), Velocity))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'angular_velocity' [X,Y,Z] (deg/s)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);
    FMCPJson::ReadBool(Request.Params, TEXT("add_to_current"), bAdd);

    auto Task = [ActorName, ComponentName, Velocity, bAdd]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->SetPhysicsAngularVelocityInDegrees(Velocity, bAdd);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        FMCPJson::WriteVec3(Result, TEXT("angular_velocity"), Velocity);
        Result->SetBoolField(TEXT("add_to_current"), bAdd);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/set_angular_velocity %s.%s (%.1f,%.1f,%.1f)"),
            *Actor->GetActorLabel(), *Prim->GetName(), Velocity.X, Velocity.Y, Velocity.Z);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPhysicsService::HandleSetMass(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    double MassKg = 0.0;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadNumber(Request.Params, TEXT("mass_kg"), MassKg))
        return InvalidParams(Request.Id, TEXT("Missing 'mass_kg' (number, kg)"));
    if (MassKg <= 0.0)
        return InvalidParams(Request.Id, TEXT("'mass_kg' must be > 0"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    auto Task = [ActorName, ComponentName, MassKg]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), /*bOverrideMass*/ true);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        Result->SetNumberField(TEXT("mass_kg"),    MassKg);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/set_mass %s.%s = %.2f kg"),
            *Actor->GetActorLabel(), *Prim->GetName(), MassKg);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPhysicsService::HandleSetCollisionEnabled(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName, ModeStr;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("mode"), ModeStr))
        return InvalidParams(Request.Id, TEXT("Missing 'mode' (NoCollision|QueryOnly|PhysicsOnly|QueryAndPhysics)"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    bool bOk = true;
    const ECollisionEnabled::Type Mode = ParseCollisionEnabled(ModeStr, bOk);
    if (!bOk)
        return InvalidParams(Request.Id, FString::Printf(TEXT("Invalid 'mode': %s"), *ModeStr));

    auto Task = [ActorName, ComponentName, Mode, ModeStr]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UPrimitiveComponent* Prim = ResolvePrimComp(Actor, ComponentName);
        if (!Prim) return FMCPJson::MakeError(TEXT("No UPrimitiveComponent on actor"));

        Prim->SetCollisionEnabled(Mode);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component"),  Prim->GetName());
        Result->SetStringField(TEXT("mode"),       ModeStr);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: physics/set_collision_enabled %s.%s = %s"),
            *Actor->GetActorLabel(), *Prim->GetName(), *ModeStr);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FPhysicsService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("set_simulate_physics"),
            TEXT("Enable or disable rigid-body simulation on an actor's primary UPrimitiveComponent. "
                 "Params: actor_name (string), simulate (bool), component_name (string, optional; defaults to root primitive). "
                 "Workflow: enable before apply_impulse/apply_force so forces have effect. "
                 "Warning: only works on components whose BodySetup allows simulation."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredBool  (TEXT("simulate"),       TEXT("true to enable, false to disable"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component name; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("apply_impulse"),
            TEXT("Apply an instantaneous impulse to a primitive component (kg*cm/s unless velocity_change=true). "
                 "Params: actor_name (string), impulse ([X,Y,Z] vector, world-space), velocity_change (bool, default false -- when true, impulse is in cm/s regardless of mass), component_name (string, optional). "
                 "Workflow: set_simulate_physics true first. "
                 "Warning: impulse is applied at center of mass; use AddImpulseAtLocation for off-center hits (not exposed here)."))
        .RequiredString(TEXT("actor_name"),      TEXT("Actor label"))
        .RequiredVec3  (TEXT("impulse"),         TEXT("Impulse as [X,Y,Z]"))
        .OptionalBool  (TEXT("velocity_change"), TEXT("Treat impulse as a velocity change (ignore mass)"))
        .OptionalString(TEXT("component_name"),  TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("apply_force"),
            TEXT("Apply a continuous force to a primitive component this tick (kg*cm/s^2 unless accel_change=true). "
                 "Params: actor_name (string), force ([X,Y,Z] vector, world-space), accel_change (bool, default false -- when true, force is an acceleration in cm/s^2), component_name (string, optional). "
                 "Workflow: set_simulate_physics true first. "
                 "Warning: a single call applies force for one tick; call every frame for sustained forces."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredVec3  (TEXT("force"),          TEXT("Force as [X,Y,Z]"))
        .OptionalBool  (TEXT("accel_change"),   TEXT("Treat force as an acceleration (ignore mass)"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_linear_velocity"),
            TEXT("Set (or add to) the linear physics velocity of a primitive component in cm/s. "
                 "Params: actor_name (string), velocity ([X,Y,Z] cm/s world-space), add_to_current (bool, default false), component_name (string, optional). "
                 "Workflow: set_simulate_physics true first."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredVec3  (TEXT("velocity"),       TEXT("Velocity as [X,Y,Z] cm/s"))
        .OptionalBool  (TEXT("add_to_current"), TEXT("Add to existing velocity instead of replacing"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_angular_velocity"),
            TEXT("Set (or add to) the angular physics velocity of a primitive component in deg/s. "
                 "Params: actor_name (string), angular_velocity ([X,Y,Z] deg/s world-space), add_to_current (bool, default false), component_name (string, optional). "
                 "Workflow: set_simulate_physics true first."))
        .RequiredString(TEXT("actor_name"),       TEXT("Actor label"))
        .RequiredVec3  (TEXT("angular_velocity"), TEXT("Angular velocity as [X,Y,Z] deg/s"))
        .OptionalBool  (TEXT("add_to_current"),   TEXT("Add to existing velocity instead of replacing"))
        .OptionalString(TEXT("component_name"),   TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_mass"),
            TEXT("Override the mass of a primitive component in kilograms. "
                 "Params: actor_name (string), mass_kg (number > 0, kilograms), component_name (string, optional). "
                 "Workflow: mass override is applied via SetMassOverrideInKg(NAME_None, kg, true). "
                 "Warning: asset-level mass settings are replaced for the instance; Blueprint defaults are unaffected."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredNumber(TEXT("mass_kg"),        TEXT("Mass in kilograms (must be > 0)"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_collision_enabled"),
            TEXT("Set ECollisionEnabled on a primitive component. "
                 "Params: actor_name (string), mode (enum: NoCollision|QueryOnly|PhysicsOnly|QueryAndPhysics), component_name (string, optional). "
                 "Workflow: NoCollision disables all collision/queries; QueryOnly allows overlaps without physics; PhysicsOnly allows physics without overlaps; QueryAndPhysics enables both."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredEnum  (TEXT("mode"),
            {TEXT("NoCollision"), TEXT("QueryOnly"), TEXT("PhysicsOnly"), TEXT("QueryAndPhysics")},
            TEXT("ECollisionEnabled mode"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    return Tools;
}

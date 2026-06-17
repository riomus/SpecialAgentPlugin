// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PhysicsService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"
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

FMCPResponse FPhysicsService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
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
            TEXT("Enable or disable rigid-body (Chaos) simulation on a primitive component (SetSimulatePhysics). "
                 "Returns {actor_name:actor_label, component, simulate:bool}. "
                 "Params: actor_name (string, required, actor label), simulate (bool, required), "
                 "component_name (string, optional; defaults to the root primitive, else the first UPrimitiveComponent found). "
                 "Workflow: REQUIRED first step before physics/apply_impulse, apply_force, set_linear_velocity, or set_angular_velocity -- forces are ignored on a non-simulating body. "
                 "Warning: the component must have Movable mobility and a BodySetup that allows simulation, or it stays static; simulation only advances while PIE/Simulate ticks (editor-world bodies do not move)."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredBool  (TEXT("simulate"),       TEXT("true to enable, false to disable"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component name; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("apply_impulse"),
            TEXT("Apply an instantaneous impulse (one-shot velocity kick) to a primitive component at its center of mass (AddImpulse). "
                 "Returns {actor_name:actor_label, component, impulse, velocity_change:bool}. "
                 "Params: actor_name (string, required, actor label), impulse (number[3], required, world-space [X,Y,Z], mass-scaled kg*cm/s by default), "
                 "velocity_change (bool, optional, default false -- when true the impulse is a direct cm/s velocity delta, mass-independent), "
                 "component_name (string, optional; defaults to root primitive). "
                 "Workflow: physics/set_simulate_physics true first; for a continuous push use physics/apply_force instead. "
                 "Warning: only moves the body while PIE/Simulate ticks; impulse acts at the center of mass (no off-center torque -- AddImpulseAtLocation is not exposed); values are cm-based, NOT real-world Newton-seconds."))
        .RequiredString(TEXT("actor_name"),      TEXT("Actor label"))
        .RequiredVec3  (TEXT("impulse"),         TEXT("Impulse as world-space [X,Y,Z] (kg*cm/s, or cm/s when velocity_change=true)"))
        .OptionalBool  (TEXT("velocity_change"), TEXT("Treat impulse as a velocity change in cm/s, ignoring mass (default false)"))
        .OptionalString(TEXT("component_name"),  TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("apply_force"),
            TEXT("Queue a continuous force on a primitive component for the next physics tick (AddForce). "
                 "Returns {actor_name:actor_label, component, force, accel_change:bool}. "
                 "Params: actor_name (string, required, actor label), force (number[3], required, world-space [X,Y,Z], mass-scaled kg*cm/s^2 by default), "
                 "accel_change (bool, optional, default false -- when true the force is an acceleration in cm/s^2, mass-independent), "
                 "component_name (string, optional; defaults to root primitive). "
                 "Workflow: physics/set_simulate_physics true first; for a one-shot kick use physics/apply_impulse instead. "
                 "Warning: only takes effect while PIE/Simulate ticks, and one call lasts a single tick -- call every frame for a sustained push; values are cm-based, NOT real-world Newtons."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredVec3  (TEXT("force"),          TEXT("Force as world-space [X,Y,Z] (kg*cm/s^2, or cm/s^2 when accel_change=true)"))
        .OptionalBool  (TEXT("accel_change"),   TEXT("Treat force as an acceleration in cm/s^2, ignoring mass (default false)"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_linear_velocity"),
            TEXT("Set (or add to) the linear physics velocity of a primitive component (SetPhysicsLinearVelocity), mass-independent. "
                 "Returns {actor_name:actor_label, component, velocity, add_to_current:bool}. "
                 "Params: actor_name (string, required, actor label), velocity (number[3], required, world-space cm/s [X,Y,Z]), "
                 "add_to_current (bool, optional, default false -- add to the existing velocity instead of overwriting it), "
                 "component_name (string, optional; defaults to root primitive). "
                 "Workflow: physics/set_simulate_physics true first. "
                 "Warning: overwrites velocity directly (ignores mass) and only persists while PIE/Simulate ticks -- a static editor-world body will not move."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredVec3  (TEXT("velocity"),       TEXT("Velocity as world-space [X,Y,Z] in cm/s"))
        .OptionalBool  (TEXT("add_to_current"), TEXT("Add to existing velocity instead of replacing (default false)"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_angular_velocity"),
            TEXT("Set (or add to) the angular physics velocity of a primitive component in degrees/sec (SetPhysicsAngularVelocityInDegrees), mass-independent. "
                 "Returns {actor_name:actor_label, component, angular_velocity, add_to_current:bool}. "
                 "Params: actor_name (string, required, actor label), angular_velocity (number[3], required, world-space deg/s about [X,Y,Z] axes), "
                 "add_to_current (bool, optional, default false -- add to the existing angular velocity instead of overwriting it), "
                 "component_name (string, optional; defaults to root primitive). "
                 "Workflow: physics/set_simulate_physics true first. "
                 "Warning: spin is in deg/s (not the [Pitch,Yaw,Roll] rotator convention), ignores mass, and only persists while PIE/Simulate ticks."))
        .RequiredString(TEXT("actor_name"),       TEXT("Actor label"))
        .RequiredVec3  (TEXT("angular_velocity"), TEXT("Angular velocity in deg/s about world [X,Y,Z] axes"))
        .OptionalBool  (TEXT("add_to_current"),   TEXT("Add to existing velocity instead of replacing (default false)"))
        .OptionalString(TEXT("component_name"),   TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_mass"),
            TEXT("Override the mass of a primitive component in kilograms (SetMassOverrideInKg(NAME_None, kg, bOverrideMass=true)). "
                 "Returns {actor_name:actor_label, component, mass_kg}. "
                 "Params: actor_name (string, required, actor label), mass_kg (number, required, kilograms, must be > 0), "
                 "component_name (string, optional; defaults to root primitive). "
                 "Workflow: scales how physics/apply_force and apply_impulse affect the body (unless their accel_change/velocity_change flag is set, which bypasses mass). "
                 "Warning: changes only this component instance, not the Blueprint/asset defaults; mass is in kg but distances are cm, so force values are not real-world Newtons."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredNumber(TEXT("mass_kg"),        TEXT("Mass in kilograms (must be > 0)"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_collision_enabled"),
            TEXT("Set the ECollisionEnabled mode on a primitive component (SetCollisionEnabled). "
                 "Returns {actor_name:actor_label, component, mode}. "
                 "Params: actor_name (string, required, actor label), "
                 "mode (enum, required: NoCollision = no collision or queries; QueryOnly = overlaps/traces but no physics blocking; PhysicsOnly = physics blocking but no traces; QueryAndPhysics = both), "
                 "component_name (string, optional; defaults to root primitive). "
                 "Workflow: PhysicsOnly/QueryAndPhysics is needed for a simulating body (physics/set_simulate_physics) to collide with the world. "
                 "Warning: changes only this component instance and only affects the editor world until saved; NoCollision lets a simulating body fall through everything."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredEnum  (TEXT("mode"),
            {TEXT("NoCollision"), TEXT("QueryOnly"), TEXT("PhysicsOnly"), TEXT("QueryAndPhysics")},
            TEXT("ECollisionEnabled mode"))
        .OptionalString(TEXT("component_name"), TEXT("Specific component; defaults to root primitive"))
        .Build());

    return Tools;
}

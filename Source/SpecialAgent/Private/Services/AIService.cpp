// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/AIService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

namespace
{
    UClass* ResolvePawnClass(const FString& ClassOrPath)
    {
        // Asset path first (Blueprint or class ref).
        if (ClassOrPath.Contains(TEXT("/")))
        {
            if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassOrPath))
            {
                if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(APawn::StaticClass()))
                {
                    return BP->GeneratedClass;
                }
            }
            if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassOrPath))
            {
                if (Loaded->IsChildOf(APawn::StaticClass()))
                {
                    return Loaded;
                }
            }
        }

        // Fallback: native class lookup by name.
        if (UClass* Found = FindFirstObject<UClass>(*ClassOrPath,
                EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous))
        {
            if (Found->IsChildOf(APawn::StaticClass()))
            {
                return Found;
            }
        }
        return nullptr;
    }

    UClass* ResolveControllerClass(const FString& ClassOrPath)
    {
        if (ClassOrPath.Contains(TEXT("/")))
        {
            if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassOrPath))
            {
                if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(AController::StaticClass()))
                {
                    return BP->GeneratedClass;
                }
            }
            if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassOrPath))
            {
                if (Loaded->IsChildOf(AController::StaticClass()))
                {
                    return Loaded;
                }
            }
        }
        if (UClass* Found = FindFirstObject<UClass>(*ClassOrPath,
                EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous))
        {
            if (Found->IsChildOf(AController::StaticClass()))
            {
                return Found;
            }
        }
        return nullptr;
    }

    AAIController* ResolveAIControllerForPawn(APawn* Pawn)
    {
        if (!Pawn) return nullptr;
        return Cast<AAIController>(Pawn->GetController());
    }
}

FString FAIService::GetServiceDescription() const
{
    return TEXT("AI pawn / controller / behavior tree / blackboard");
}

FMCPResponse FAIService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("spawn_ai_pawn"))        return HandleSpawnAIPawn(Request);
    if (MethodName == TEXT("assign_controller"))    return HandleAssignController(Request);
    if (MethodName == TEXT("run_behavior_tree"))    return HandleRunBehaviorTree(Request);
    if (MethodName == TEXT("set_blackboard_value")) return HandleSetBlackboardValue(Request);
    if (MethodName == TEXT("stop_ai"))              return HandleStopAI(Request);

    return MethodNotFound(Request.Id, TEXT("ai"), MethodName);
}

FMCPResponse FAIService::HandleSpawnAIPawn(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString PawnClass;
    if (!FMCPJson::ReadString(Request.Params, TEXT("pawn_class"), PawnClass) || PawnClass.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'pawn_class' (asset path or class name)"));

    FVector Location(0, 0, 0);
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' ([X, Y, Z])"));

    FRotator Rotation(0, 0, 0);
    FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);

    bool bAutoPossess = true;
    FMCPJson::ReadBool(Request.Params, TEXT("auto_possess"), bAutoPossess);

    auto Task = [PawnClass, Location, Rotation, bAutoPossess]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        UClass* Class = ResolvePawnClass(PawnClass);
        if (!Class)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Failed to resolve pawn class: %s (must be APawn subclass)"), *PawnClass));
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        APawn* NewPawn = World->SpawnActor<APawn>(Class, Location, Rotation, SpawnParams);
        if (!NewPawn)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("SpawnActor<APawn> returned null for class: %s"), *PawnClass));
        }

        if (bAutoPossess)
        {
            NewPawn->SpawnDefaultController();
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        TSharedPtr<FJsonObject> PawnData = MakeShared<FJsonObject>();
        FMCPJson::WriteActor(PawnData, NewPawn);
        Result->SetObjectField(TEXT("pawn"), PawnData);

        if (AController* Controller = NewPawn->GetController())
        {
            Result->SetStringField(TEXT("controller_class"), Controller->GetClass()->GetName());
            Result->SetStringField(TEXT("controller_name"),  Controller->GetName());
        }
        else
        {
            Result->SetStringField(TEXT("controller_class"), TEXT(""));
        }

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Spawned AI pawn %s from %s (auto_possess=%s)"),
            *NewPawn->GetActorLabel(), *PawnClass, bAutoPossess ? TEXT("true") : TEXT("false"));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAIService::HandleAssignController(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString PawnName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("pawn_name"), PawnName) || PawnName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'pawn_name'"));

    FString ControllerClass;
    FMCPJson::ReadString(Request.Params, TEXT("controller_class"), ControllerClass);

    auto Task = [PawnName, ControllerClass]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, PawnName);
        APawn* Pawn = Cast<APawn>(Actor);
        if (!Pawn)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Actor '%s' not found or is not a Pawn"), *PawnName));
        }

        // Unpossess any existing controller before assigning a new one.
        if (AController* Existing = Pawn->GetController())
        {
            Existing->UnPossess();
            Existing->Destroy();
        }

        AController* NewController = nullptr;

        if (ControllerClass.IsEmpty())
        {
            Pawn->SpawnDefaultController();
            NewController = Pawn->GetController();
        }
        else
        {
            UClass* ControllerCls = ResolveControllerClass(ControllerClass);
            if (!ControllerCls)
            {
                return FMCPJson::MakeError(FString::Printf(
                    TEXT("Failed to resolve controller class: %s"), *ControllerClass));
            }

            FActorSpawnParameters SpawnParams;
            SpawnParams.Instigator = Pawn->GetInstigator();
            SpawnParams.OverrideLevel = Pawn->GetLevel();
            SpawnParams.ObjectFlags |= RF_Transient;
            NewController = World->SpawnActor<AController>(ControllerCls,
                Pawn->GetActorLocation(), Pawn->GetActorRotation(), SpawnParams);
            if (NewController)
            {
                NewController->Possess(Pawn);
            }
        }

        if (!NewController)
        {
            return FMCPJson::MakeError(TEXT("Failed to spawn/assign controller"));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("pawn"), Pawn->GetActorLabel());
        Result->SetStringField(TEXT("controller_class"), NewController->GetClass()->GetName());
        Result->SetStringField(TEXT("controller_name"),  NewController->GetName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Assigned controller %s to pawn %s"),
            *NewController->GetClass()->GetName(), *Pawn->GetActorLabel());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAIService::HandleRunBehaviorTree(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString PawnName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("pawn_name"), PawnName) || PawnName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'pawn_name'"));

    FString BTPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("behavior_tree"), BTPath) || BTPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'behavior_tree' (asset path)"));

    auto Task = [PawnName, BTPath]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, PawnName);
        APawn* Pawn = Cast<APawn>(Actor);
        if (!Pawn)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Actor '%s' not found or is not a Pawn"), *PawnName));
        }

        AAIController* AICon = ResolveAIControllerForPawn(Pawn);
        if (!AICon)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Pawn '%s' has no AAIController. Call ai/assign_controller first."), *PawnName));
        }

        UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *BTPath);
        if (!BT)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Failed to load BehaviorTree asset: %s"), *BTPath));
        }

        const bool bOk = AICon->RunBehaviorTree(BT);
        if (!bOk)
        {
            return FMCPJson::MakeError(TEXT("AAIController::RunBehaviorTree returned false"));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("pawn"), Pawn->GetActorLabel());
        Result->SetStringField(TEXT("controller"), AICon->GetName());
        Result->SetStringField(TEXT("behavior_tree"), BT->GetPathName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Running BT '%s' on pawn '%s'"),
            *BT->GetName(), *Pawn->GetActorLabel());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAIService::HandleSetBlackboardValue(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString PawnName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("pawn_name"), PawnName) || PawnName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'pawn_name'"));

    FString Key;
    if (!FMCPJson::ReadString(Request.Params, TEXT("key"), Key) || Key.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'key' (blackboard key name)"));

    FString ValueType;
    if (!FMCPJson::ReadString(Request.Params, TEXT("value_type"), ValueType) || ValueType.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'value_type' (bool|int|float|vector|object|string|name)"));

    ValueType = ValueType.ToLower();

    if (!Request.Params->HasField(TEXT("value")))
        return InvalidParams(Request.Id, TEXT("Missing 'value' field"));

    bool     BoolV   = false;
    int32    IntV    = 0;
    double   FloatV  = 0.0;
    FVector  VecV    = FVector::ZeroVector;
    FString  StrV;
    FString  ObjectName;

    if      (ValueType == TEXT("bool"))   { if (!FMCPJson::ReadBool(Request.Params,    TEXT("value"), BoolV))      return InvalidParams(Request.Id, TEXT("'value' must be bool")); }
    else if (ValueType == TEXT("int"))    { if (!FMCPJson::ReadInteger(Request.Params, TEXT("value"), IntV))       return InvalidParams(Request.Id, TEXT("'value' must be integer")); }
    else if (ValueType == TEXT("float"))  { if (!FMCPJson::ReadNumber(Request.Params,  TEXT("value"), FloatV))     return InvalidParams(Request.Id, TEXT("'value' must be number")); }
    else if (ValueType == TEXT("vector")) { if (!FMCPJson::ReadVec3(Request.Params,    TEXT("value"), VecV))       return InvalidParams(Request.Id, TEXT("'value' must be [X, Y, Z]")); }
    else if (ValueType == TEXT("string")) { if (!FMCPJson::ReadString(Request.Params,  TEXT("value"), StrV))       return InvalidParams(Request.Id, TEXT("'value' must be string")); }
    else if (ValueType == TEXT("name"))   { if (!FMCPJson::ReadString(Request.Params,  TEXT("value"), StrV))       return InvalidParams(Request.Id, TEXT("'value' must be string (converted to FName)")); }
    else if (ValueType == TEXT("object")) { if (!FMCPJson::ReadString(Request.Params,  TEXT("value"), ObjectName)) return InvalidParams(Request.Id, TEXT("'value' must be actor label string")); }
    else
    {
        return InvalidParams(Request.Id,
            TEXT("'value_type' must be one of: bool, int, float, vector, object, string, name"));
    }

    auto Task = [PawnName, Key, ValueType, BoolV, IntV, FloatV, VecV, StrV, ObjectName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, PawnName);
        APawn* Pawn = Cast<APawn>(Actor);
        if (!Pawn)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Actor '%s' not found or is not a Pawn"), *PawnName));
        }

        AAIController* AICon = ResolveAIControllerForPawn(Pawn);
        if (!AICon)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Pawn '%s' has no AAIController"), *PawnName));
        }

        UBlackboardComponent* BB = AICon->GetBlackboardComponent();
        if (!BB)
        {
            return FMCPJson::MakeError(TEXT("AIController has no blackboard component (run a BT first)"));
        }

        const FName KeyName(*Key);

        if      (ValueType == TEXT("bool"))   BB->SetValueAsBool  (KeyName, BoolV);
        else if (ValueType == TEXT("int"))    BB->SetValueAsInt   (KeyName, IntV);
        else if (ValueType == TEXT("float"))  BB->SetValueAsFloat (KeyName, static_cast<float>(FloatV));
        else if (ValueType == TEXT("vector")) BB->SetValueAsVector(KeyName, VecV);
        else if (ValueType == TEXT("string")) BB->SetValueAsString(KeyName, StrV);
        else if (ValueType == TEXT("name"))   BB->SetValueAsName  (KeyName, FName(*StrV));
        else if (ValueType == TEXT("object"))
        {
            AActor* ValueActor = FMCPActorResolver::ByLabel(World, ObjectName);
            if (!ValueActor)
            {
                return FMCPJson::MakeError(FString::Printf(
                    TEXT("Object value: actor '%s' not found"), *ObjectName));
            }
            BB->SetValueAsObject(KeyName, ValueActor);
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("pawn"), Pawn->GetActorLabel());
        Result->SetStringField(TEXT("key"), Key);
        Result->SetStringField(TEXT("value_type"), ValueType);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Set blackboard '%s' on '%s' (%s)"),
            *Key, *Pawn->GetActorLabel(), *ValueType);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAIService::HandleStopAI(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString PawnName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("pawn_name"), PawnName) || PawnName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'pawn_name'"));

    FString Reason = TEXT("ai/stop_ai MCP request");
    FMCPJson::ReadString(Request.Params, TEXT("reason"), Reason);

    bool bUnpossess = false;
    FMCPJson::ReadBool(Request.Params, TEXT("unpossess"), bUnpossess);

    auto Task = [PawnName, Reason, bUnpossess]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, PawnName);
        APawn* Pawn = Cast<APawn>(Actor);
        if (!Pawn)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Actor '%s' not found or is not a Pawn"), *PawnName));
        }

        AAIController* AICon = ResolveAIControllerForPawn(Pawn);
        if (!AICon)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Pawn '%s' has no AAIController"), *PawnName));
        }

        bool bStoppedLogic = false;
        if (UBrainComponent* Brain = AICon->GetBrainComponent())
        {
            Brain->StopLogic(Reason);
            bStoppedLogic = true;
        }

        if (bUnpossess)
        {
            AICon->UnPossess();
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("pawn"), Pawn->GetActorLabel());
        Result->SetBoolField(TEXT("stopped_logic"), bStoppedLogic);
        Result->SetBoolField(TEXT("unpossessed"), bUnpossess);
        Result->SetStringField(TEXT("reason"), Reason);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Stopped AI on pawn '%s' (unpossess=%s, reason=%s)"),
            *Pawn->GetActorLabel(), bUnpossess ? TEXT("true") : TEXT("false"), *Reason);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FAIService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("spawn_ai_pawn"),
            TEXT("Spawn a Pawn into the editor world and optionally let it spawn its default AIController. "
                 "Returns {pawn:{label,name,transform,...}, controller_class, controller_name} (controller_class empty when unpossessed). "
                 "Params: pawn_class (string, required, virtual asset path /Game/AI/BP_Enemy.BP_Enemy or native APawn-subclass name), "
                 "location (number[3], required, world-space cm [X,Y,Z]), "
                 "rotation (number[3], optional, degrees [Pitch,Yaw,Roll]; the Details panel labels these X=Roll/Y=Pitch/Z=Yaw -- order differs), "
                 "auto_possess (bool, optional, default true -- calls Pawn->SpawnDefaultController()). "
                 "Workflow: spawn -> ai/assign_controller (for a custom AIController) -> ai/run_behavior_tree to start logic. "
                 "Warning: pawn_class must resolve to an APawn subclass or this returns an error; the pawn is spawned in the persistent editor level (not saved to disk here) and does NOT tick until PIE runs, so AI/movement only animates in Play-In-Editor."))
        .RequiredString(TEXT("pawn_class"),   TEXT("Virtual asset path (e.g. /Game/AI/BP_Enemy.BP_Enemy) or native class name of an APawn subclass"))
        .RequiredVec3  (TEXT("location"),     TEXT("Spawn location [X, Y, Z] in cm, world space"))
        .OptionalVec3  (TEXT("rotation"),     TEXT("Rotation [Pitch, Yaw, Roll] in degrees (default [0,0,0])"))
        .OptionalBool  (TEXT("auto_possess"), TEXT("If true, call Pawn->SpawnDefaultController() after spawn (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("assign_controller"),
            TEXT("Spawn and possess an AController (or subclass) on an existing Pawn, replacing whatever controller it had. "
                 "Returns {pawn:actor_label, controller_class, controller_name}. "
                 "Params: pawn_name (string, required, actor label of the Pawn), "
                 "controller_class (string, optional, virtual asset path or native AController-subclass name; empty/omitted = Pawn->SpawnDefaultController). "
                 "Workflow: required before ai/run_behavior_tree and ai/set_blackboard_value, which need an AAIController. "
                 "Warning: the existing controller is unpossessed AND destroyed first; the new controller is spawned transient (not saved with the level) and only drives the pawn once PIE is ticking."))
        .RequiredString(TEXT("pawn_name"),        TEXT("Actor label of the Pawn to possess"))
        .OptionalString(TEXT("controller_class"), TEXT("Virtual asset path or native name of an AController subclass; empty = SpawnDefaultController"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("run_behavior_tree"),
            TEXT("Load a UBehaviorTree asset and run it on a Pawn's AAIController (AAIController::RunBehaviorTree). "
                 "Returns {pawn:actor_label, controller, behavior_tree:object_path}. "
                 "Params: pawn_name (string, required, actor label), "
                 "behavior_tree (string, required, virtual asset path /Game/AI/BT_Enemy.BT_Enemy). "
                 "Workflow: ai/assign_controller first (pawn must be possessed by an AAIController); the BT's referenced Blackboard provides the keys for ai/set_blackboard_value. "
                 "Warning: replaces any BT already running on that controller; StateTree is Epic's recommended forward path but BehaviorTrees still run in 5.7. AI only moves while PIE ticks and only if a NavMeshBoundsVolume + built RecastNavMesh cover the area (see navigation/rebuild_navmesh)."))
        .RequiredString(TEXT("pawn_name"),     TEXT("Actor label of the Pawn whose AIController will run the BT"))
        .RequiredString(TEXT("behavior_tree"), TEXT("Virtual asset path to the UBehaviorTree (e.g. /Game/AI/BT_Enemy.BT_Enemy)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_blackboard_value"),
            TEXT("Write a typed value to a key on a pawn's AIController Blackboard via the matching SetValueAs* call. "
                 "Returns {pawn:actor_label, key, value_type}. "
                 "Params: pawn_name (string, required, actor label), key (string, required, Blackboard key name), "
                 "value_type (enum, required: bool|int|float|vector|object|string|name -- selects the SetValueAs* overload), "
                 "value (required; JSON type must match value_type: bool->boolean, int->integer, float->number, "
                 "vector->[X,Y,Z] number array in cm, string/name->string, object->actor-label string resolved against the editor world). "
                 "Workflow: run ai/run_behavior_tree first so the AIController has a Blackboard component (this errors if none exists). "
                 "Warning: the key must already be defined on the Blackboard with a matching type -- SetValueAs* on an unknown key is a no-op, and a type mismatch writes nothing. object value errors if the named actor is not found."))
        .RequiredString(TEXT("pawn_name"),  TEXT("Actor label of the Pawn whose AIController owns the blackboard"))
        .RequiredString(TEXT("key"),        TEXT("Blackboard key name (must already exist with a matching type)"))
        .RequiredEnum  (TEXT("value_type"), { TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("vector"), TEXT("object"), TEXT("string"), TEXT("name") },
                                             TEXT("Value type that drives which SetValueAs* overload is called"))
        .RequiredAny   (TEXT("value"),      TEXT("Value whose JSON type must match value_type: "
                                                 "bool -> boolean; int -> integer; float -> number; "
                                                 "vector -> [X, Y, Z] number array (cm); "
                                                 "string/name -> string; "
                                                 "object -> actor-label string (resolved against the editor world)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("stop_ai"),
            TEXT("Stop the logic (BehaviorTree/StateTree) running on a Pawn's AIController via BrainComponent::StopLogic, optionally unpossess it. "
                 "Returns {pawn:actor_label, stopped_logic:bool, unpossessed:bool, reason}. "
                 "Params: pawn_name (string, required, actor label), "
                 "reason (string, optional, log tag forwarded to StopLogic, default \"ai/stop_ai MCP request\"), "
                 "unpossess (bool, optional, default false -- also calls AIController::UnPossess). "
                 "Workflow: inverse of ai/run_behavior_tree; re-run that tool to resume logic. "
                 "Warning: errors if the pawn has no AAIController; unpossess=true detaches the controller (the pawn is left uncontrolled) without spawning a replacement."))
        .RequiredString(TEXT("pawn_name"), TEXT("Actor label of the Pawn whose AI is being stopped"))
        .OptionalString(TEXT("reason"),    TEXT("Human-readable reason string forwarded to StopLogic() (default: \"ai/stop_ai MCP request\")"))
        .OptionalBool  (TEXT("unpossess"), TEXT("If true, also call AIController::UnPossess() (default false)"))
        .Build());

    return Tools;
}

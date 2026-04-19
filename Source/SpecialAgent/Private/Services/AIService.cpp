// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/AIService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

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

FMCPResponse FAIService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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
            TEXT("Spawn a Pawn actor and optionally assign its default AIController. Result includes serialized pawn transform. "
                 "Params: pawn_class (string, asset path or native class name), location ([X,Y,Z] cm world), "
                 "rotation (optional [Pitch,Yaw,Roll] deg), auto_possess (bool, default true). "
                 "Workflow: follow with ai/run_behavior_tree to start AI logic. "
                 "Warning: pawn_class must resolve to an APawn subclass; auto_possess=false leaves the pawn unpossessed."))
        .RequiredString(TEXT("pawn_class"),   TEXT("Blueprint/class path (e.g. /Game/AI/BP_Enemy.BP_Enemy) or native name of an APawn subclass"))
        .RequiredVec3  (TEXT("location"),     TEXT("Spawn location [X, Y, Z] in cm (world space)"))
        .OptionalVec3  (TEXT("rotation"),     TEXT("Rotation [Pitch, Yaw, Roll] in degrees"))
        .OptionalBool  (TEXT("auto_possess"), TEXT("If true, call Pawn->SpawnDefaultController() after spawn (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("assign_controller"),
            TEXT("Attach an AIController (or subclass) to an existing Pawn, replacing any current controller. "
                 "Params: pawn_name (string, actor label), controller_class (string, optional asset path/class name; empty = Pawn->SpawnDefaultController). "
                 "Workflow: precondition for ai/run_behavior_tree and ai/set_blackboard_value. "
                 "Warning: destroys the previous controller."))
        .RequiredString(TEXT("pawn_name"),        TEXT("Actor label of the Pawn to possess"))
        .OptionalString(TEXT("controller_class"), TEXT("Asset path or native name of AController subclass; empty = use default controller"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("run_behavior_tree"),
            TEXT("Start a BehaviorTree asset on a Pawn's AIController via AAIController::RunBehaviorTree. "
                 "Params: pawn_name (string, actor label), behavior_tree (string, asset path). "
                 "Workflow: requires a Pawn possessed by an AAIController (see ai/assign_controller). "
                 "Warning: resets any BT currently running on that controller."))
        .RequiredString(TEXT("pawn_name"),     TEXT("Actor label of the Pawn whose AIController will run the BT"))
        .RequiredString(TEXT("behavior_tree"), TEXT("Asset path to the UBehaviorTree (e.g. /Game/AI/BT_Enemy.BT_Enemy)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_blackboard_value"),
            TEXT("Write a typed value to a pawn's blackboard key via SetValueAs*. "
                 "Params: pawn_name (string), key (string, blackboard key name), value_type (enum bool|int|float|vector|object|string|name), value (typed). "
                 "For 'object', value is an actor label resolved via the editor world. "
                 "Workflow: call after ai/run_behavior_tree so the blackboard component exists. "
                 "Warning: key type must match the blackboard definition; setting a missing key is a silent no-op."))
        .RequiredString(TEXT("pawn_name"),  TEXT("Actor label of the Pawn whose AIController owns the blackboard"))
        .RequiredString(TEXT("key"),        TEXT("Blackboard key name"))
        .RequiredEnum  (TEXT("value_type"), { TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("vector"), TEXT("object"), TEXT("string"), TEXT("name") },
                                             TEXT("Value type — drives which SetValueAs* overload is called"))
        .RequiredString(TEXT("value"),      TEXT("Value — bool/number/[X,Y,Z]/string per value_type; for 'object' use an actor label"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("stop_ai"),
            TEXT("Stop the behavior tree running on a Pawn's AIController via BrainComponent::StopLogic, optionally unpossess. "
                 "Params: pawn_name (string), reason (string, optional log tag), unpossess (bool, default false). "
                 "Workflow: inverse of ai/run_behavior_tree. "
                 "Warning: unpossess=true detaches the controller without spawning a replacement."))
        .RequiredString(TEXT("pawn_name"), TEXT("Actor label of the Pawn whose AI is being stopped"))
        .OptionalString(TEXT("reason"),    TEXT("Human-readable reason string forwarded to StopLogic() (default: \"ai/stop_ai MCP request\")"))
        .OptionalBool  (TEXT("unpossess"), TEXT("If true, also call AIController::UnPossess() (default false)"))
        .Build());

    return Tools;
}

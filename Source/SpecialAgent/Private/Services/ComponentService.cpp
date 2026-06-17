// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ComponentService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "ScopedTransaction.h"

namespace
{
    UActorComponent* FindComponentOnActor(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor) return nullptr;
        for (UActorComponent* Comp : Actor->GetComponents())
        {
            if (!Comp) continue;
            if (Comp->GetName() == ComponentName || Comp->GetFName().ToString() == ComponentName)
            {
                return Comp;
            }
        }
        return nullptr;
    }

    UClass* ResolveComponentClass(const FString& ClassNameOrPath)
    {
        // Try as direct UObject path (blueprint-generated classes)
        if (ClassNameOrPath.Contains(TEXT("/")))
        {
            if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassNameOrPath))
            {
                if (Loaded->IsChildOf(UActorComponent::StaticClass()))
                {
                    return Loaded;
                }
            }
        }
        // Native class by name (with or without the "U" prefix)
        UClass* Found = FindFirstObject<UClass>(*ClassNameOrPath, EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous);
        if (Found && Found->IsChildOf(UActorComponent::StaticClass()))
        {
            return Found;
        }
        return nullptr;
    }

    EAttachmentRule ParseAttachmentRule(const FString& Str, EAttachmentRule Default)
    {
        if (Str.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase)) return EAttachmentRule::KeepRelative;
        if (Str.Equals(TEXT("KeepWorld"),    ESearchCase::IgnoreCase)) return EAttachmentRule::KeepWorld;
        if (Str.Equals(TEXT("SnapToTarget"), ESearchCase::IgnoreCase)) return EAttachmentRule::SnapToTarget;
        return Default;
    }

    EDetachmentRule ParseDetachmentRule(const FString& Str, EDetachmentRule Default)
    {
        if (Str.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase)) return EDetachmentRule::KeepRelative;
        if (Str.Equals(TEXT("KeepWorld"),    ESearchCase::IgnoreCase)) return EDetachmentRule::KeepWorld;
        return Default;
    }
}

FString FComponentService::GetServiceDescription() const
{
    return TEXT("Actor component add/remove/query/property editing");
}

FMCPResponse FComponentService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("add"))            return HandleAdd(Request);
    if (MethodName == TEXT("remove"))         return HandleRemove(Request);
    if (MethodName == TEXT("list"))           return HandleList(Request);
    if (MethodName == TEXT("get_properties")) return HandleGetProperties(Request);
    if (MethodName == TEXT("set_property"))   return HandleSetProperty(Request);
    if (MethodName == TEXT("attach"))         return HandleAttach(Request);
    if (MethodName == TEXT("detach"))         return HandleDetach(Request);

    return MethodNotFound(Request.Id, TEXT("component"), MethodName);
}

FMCPResponse FComponentService::HandleAdd(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentClass, ComponentName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("component_class"), ComponentClass))
        return InvalidParams(Request.Id, TEXT("Missing 'component_class'"));
    FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName);

    auto Task = [ActorName, ComponentClass, ComponentName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UClass* Class = ResolveComponentClass(ComponentClass);
        if (!Class) return FMCPJson::MakeError(FString::Printf(TEXT("Component class not found or not a UActorComponent subclass: %s"), *ComponentClass));

        FName CompFName = NAME_None;
        if (!ComponentName.IsEmpty())
        {
            CompFName = MakeUniqueObjectName(Actor, Class, FName(*ComponentName));
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Add Component")));
        Actor->Modify();

        UActorComponent* NewComp = NewObject<UActorComponent>(Actor, Class, CompFName);
        if (!NewComp) return FMCPJson::MakeError(TEXT("NewObject returned null"));

        Actor->AddInstanceComponent(NewComp);
        NewComp->RegisterComponent();

        // Attach scene components to root so they have a transform
        if (USceneComponent* Scene = Cast<USceneComponent>(NewComp))
        {
            if (USceneComponent* Root = Actor->GetRootComponent())
            {
                Scene->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
            }
        }

        NewComp->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("component_name"), NewComp->GetName());
        Result->SetStringField(TEXT("component_class"), NewComp->GetClass()->GetName());
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/add %s (%s) on %s"),
            *NewComp->GetName(), *NewComp->GetClass()->GetName(), *Actor->GetActorLabel());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FComponentService::HandleRemove(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName))
        return InvalidParams(Request.Id, TEXT("Missing 'component_name'"));

    auto Task = [ActorName, ComponentName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UActorComponent* Comp = FindComponentOnActor(Actor, ComponentName);
        if (!Comp) return FMCPJson::MakeError(FString::Printf(TEXT("Component not found: %s"), *ComponentName));

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Remove Component")));
        Actor->Modify();
        Comp->Modify();

        Actor->RemoveInstanceComponent(Comp);
        Comp->DestroyComponent();

        Actor->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("component_name"), ComponentName);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/remove %s on %s"), *ComponentName, *Actor->GetActorLabel());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FComponentService::HandleList(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

    auto Task = [ActorName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        TArray<TSharedPtr<FJsonValue>> CompsJson;
        for (UActorComponent* Comp : Actor->GetComponents())
        {
            if (!Comp) continue;
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"),  Comp->GetName());
            Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
            Obj->SetBoolField  (TEXT("is_scene_component"), Comp->IsA(USceneComponent::StaticClass()));
            CompsJson.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetArrayField(TEXT("components"), CompsJson);
        Result->SetNumberField(TEXT("count"), CompsJson.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/list %s -> %d components"), *Actor->GetActorLabel(), CompsJson.Num());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FComponentService::HandleGetProperties(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName))
        return InvalidParams(Request.Id, TEXT("Missing 'component_name'"));

    auto Task = [ActorName, ComponentName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UActorComponent* Comp = FindComponentOnActor(Actor, ComponentName);
        if (!Comp) return FMCPJson::MakeError(FString::Printf(TEXT("Component not found: %s"), *ComponentName));

        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> PropIt(Comp->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop) continue;
            FString ValueStr;
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
            Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Comp, PPF_None);

            TSharedPtr<FJsonObject> PropEntry = MakeShared<FJsonObject>();
            PropEntry->SetStringField(TEXT("type"),  Prop->GetCPPType());
            PropEntry->SetStringField(TEXT("value"), ValueStr);
            PropsObj->SetObjectField(Prop->GetName(), PropEntry);
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("component_name"),  Comp->GetName());
        Result->SetStringField(TEXT("component_class"), Comp->GetClass()->GetName());
        Result->SetObjectField(TEXT("properties"),      PropsObj);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/get_properties %s on %s"), *Comp->GetName(), *Actor->GetActorLabel());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FComponentService::HandleSetProperty(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ComponentName, PropertyName, Value;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("component_name"), ComponentName))
        return InvalidParams(Request.Id, TEXT("Missing 'component_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("property_name"), PropertyName))
        return InvalidParams(Request.Id, TEXT("Missing 'property_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("value"), Value))
        return InvalidParams(Request.Id, TEXT("Missing 'value' (string)"));

    auto Task = [ActorName, ComponentName, PropertyName, Value]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        UActorComponent* Comp = FindComponentOnActor(Actor, ComponentName);
        if (!Comp) return FMCPJson::MakeError(FString::Printf(TEXT("Component not found: %s"), *ComponentName));

        FProperty* Prop = Comp->GetClass()->FindPropertyByName(FName(*PropertyName));
        if (!Prop) return FMCPJson::MakeError(FString::Printf(TEXT("Property not found: %s"), *PropertyName));

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Component Property")));
        Comp->Modify();
        void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
        const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Comp, PPF_None);
        if (ImportResult == nullptr)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("ImportText_Direct failed for '%s' on property '%s'"), *Value, *PropertyName));
        }

        FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
        Comp->PostEditChangeProperty(ChangeEvent);
        Comp->MarkPackageDirty();

        TSharedPtr<FJsonObject> Res = FMCPJson::MakeSuccess();
        Res->SetStringField(TEXT("component_name"), Comp->GetName());
        Res->SetStringField(TEXT("property_name"),  PropertyName);
        Res->SetStringField(TEXT("value"),          Value);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/set_property %s.%s=%s"), *Comp->GetName(), *PropertyName, *Value);
        return Res;
    };

    TSharedPtr<FJsonObject> ResultObj = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, ResultObj);
}

FMCPResponse FComponentService::HandleAttach(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ChildName, ParentName;
    FString LocationRuleStr = TEXT("KeepRelative");
    FString RotationRuleStr = TEXT("KeepRelative");
    FString ScaleRuleStr    = TEXT("KeepRelative");
    FString SocketName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("child_component"), ChildName))
        return InvalidParams(Request.Id, TEXT("Missing 'child_component'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("parent_component"), ParentName))
        return InvalidParams(Request.Id, TEXT("Missing 'parent_component'"));
    FMCPJson::ReadString(Request.Params, TEXT("location_rule"), LocationRuleStr);
    FMCPJson::ReadString(Request.Params, TEXT("rotation_rule"), RotationRuleStr);
    FMCPJson::ReadString(Request.Params, TEXT("scale_rule"),    ScaleRuleStr);
    FMCPJson::ReadString(Request.Params, TEXT("socket"),        SocketName);

    auto Task = [ActorName, ChildName, ParentName, LocationRuleStr, RotationRuleStr, ScaleRuleStr, SocketName]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        USceneComponent* Child  = Cast<USceneComponent>(FindComponentOnActor(Actor, ChildName));
        USceneComponent* Parent = Cast<USceneComponent>(FindComponentOnActor(Actor, ParentName));
        if (!Child)  return FMCPJson::MakeError(FString::Printf(TEXT("Child scene component not found: %s"), *ChildName));
        if (!Parent) return FMCPJson::MakeError(FString::Printf(TEXT("Parent scene component not found: %s"), *ParentName));

        FAttachmentTransformRules Rules(
            ParseAttachmentRule(LocationRuleStr, EAttachmentRule::KeepRelative),
            ParseAttachmentRule(RotationRuleStr, EAttachmentRule::KeepRelative),
            ParseAttachmentRule(ScaleRuleStr,    EAttachmentRule::KeepRelative),
            /*bWeldSimulatedBodies*/ false);

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Attach Component")));
        Child->Modify();

        const bool bAttached = Child->AttachToComponent(Parent, Rules, SocketName.IsEmpty() ? NAME_None : FName(*SocketName));

        Child->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetBoolField  (TEXT("attached"), bAttached);
        Result->SetStringField(TEXT("child"),    Child->GetName());
        Result->SetStringField(TEXT("parent"),   Parent->GetName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/attach %s -> %s (attached=%d)"), *Child->GetName(), *Parent->GetName(), bAttached ? 1 : 0);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FComponentService::HandleDetach(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActorName, ChildName;
    FString LocationRuleStr = TEXT("KeepWorld");
    FString RotationRuleStr = TEXT("KeepWorld");
    FString ScaleRuleStr    = TEXT("KeepWorld");
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("child_component"), ChildName))
        return InvalidParams(Request.Id, TEXT("Missing 'child_component'"));
    FMCPJson::ReadString(Request.Params, TEXT("location_rule"), LocationRuleStr);
    FMCPJson::ReadString(Request.Params, TEXT("rotation_rule"), RotationRuleStr);
    FMCPJson::ReadString(Request.Params, TEXT("scale_rule"),    ScaleRuleStr);

    auto Task = [ActorName, ChildName, LocationRuleStr, RotationRuleStr, ScaleRuleStr]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));

        USceneComponent* Child = Cast<USceneComponent>(FindComponentOnActor(Actor, ChildName));
        if (!Child) return FMCPJson::MakeError(FString::Printf(TEXT("Child scene component not found: %s"), *ChildName));

        FDetachmentTransformRules Rules(
            ParseDetachmentRule(LocationRuleStr, EDetachmentRule::KeepWorld),
            ParseDetachmentRule(RotationRuleStr, EDetachmentRule::KeepWorld),
            ParseDetachmentRule(ScaleRuleStr,    EDetachmentRule::KeepWorld),
            /*bCallModify*/ true);

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Detach Component")));
        Child->Modify();

        Child->DetachFromComponent(Rules);

        Child->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("child"), Child->GetName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: component/detach %s"), *Child->GetName());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FComponentService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("add"),
            TEXT("Add an instance component to a placed actor in the editor world. Returns {component_name (resolved, may be uniquified), component_class, actor_name}. "
                 "Params: actor_name (string, required, actor LABEL as shown in the World Outliner, not the internal name), component_class (string, required, native class short name like 'StaticMeshComponent' / 'PointLightComponent', or a /Game/... path to a Blueprint-generated component class), component_name (string, optional; auto-uniquified if it collides). "
                 "Workflow: find labels via the world/actor listing tool, then component/list to confirm the add; scene components auto-attach to the actor root with KeepRelativeTransform. "
                 "Warning: this is a transient INSTANCE component (AddInstanceComponent + RegisterComponent), not a persistent Blueprint subobject -- it is lost on level reload unless the level is saved, and it is the wrong tool for authoring components into a Blueprint class."))
        .RequiredString(TEXT("actor_name"),      TEXT("Actor label as shown in the World Outliner"))
        .RequiredString(TEXT("component_class"), TEXT("Native class short name or /Game/... path to a component class"))
        .OptionalString(TEXT("component_name"),  TEXT("Desired component name; auto-uniquified on collision"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("remove"),
            TEXT("Remove an instance component from a placed actor (RemoveInstanceComponent + DestroyComponent). Returns {actor_name, component_name}. "
                 "Params: actor_name (string, required, actor LABEL from the World Outliner), component_name (string, required, exact name as reported by component/list). "
                 "Workflow: call component/list first to copy the exact component name. "
                 "Warning: undoable (wrapped in an undo transaction); destroying the root scene component or a component the actor depends on can leave the actor in a broken state -- avoid targeting the root."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label as shown in the World Outliner"))
        .RequiredString(TEXT("component_name"), TEXT("Exact component name from component/list"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("list"),
            TEXT("List every component currently on a placed actor. Returns {components[{name, class, is_scene_component (bool)}], count}. "
                 "Params: actor_name (string, required, actor LABEL from the World Outliner). Read-only, no side effects. "
                 "Workflow: run this first to get exact component names for component/get_properties, set_property, attach, detach, or remove. "
                 "Warning: reflects live editor-world state, so it includes both native components and any instance components added this session; only entries with is_scene_component=true can be attached/detached."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label as shown in the World Outliner"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("get_properties"),
            TEXT("Read all reflected properties of a component as strings. Returns {component_name, component_class, properties{<prop_name>: {type (C++ type, e.g. 'FVector'), value (stringified)}}}. "
                 "Params: actor_name (string, required, actor LABEL), component_name (string, required, exact name from component/list). Read-only, no side effects. "
                 "Workflow: read here to learn exact property names and current values, then round-trip an edit through component/set_property. "
                 "Warning: every value is stringified via ExportTextItem (e.g. FVector as '(X=0,Y=0,Z=0)' in cm, FRotator in degrees); struct and array payloads can be large/verbose."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label as shown in the World Outliner"))
        .RequiredString(TEXT("component_name"), TEXT("Exact component name from component/list"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_property"),
            TEXT("Set one reflected property on a component (ImportText + PostEditChangeProperty). Returns {component_name, property_name, value}. "
                 "Params: actor_name (string, required, actor LABEL), component_name (string, required, exact name from component/list), property_name (string, required, exact reflected name), value (string, required, UE ImportText form: '(X=1,Y=2,Z=3)' FVector in cm, '(Pitch=0,Yaw=90,Roll=0)' FRotator in degrees, 'true'/'false' bool, plain number for int/float). "
                 "Workflow: call component/get_properties first to get exact names and the current value form to mirror. "
                 "Warning: edits the live editor-world component only and does NOT save the level -- changes are lost on reload unless the level is saved. Malformed text is rejected, but a well-formed but semantically wrong value still applies silently."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label as shown in the World Outliner"))
        .RequiredString(TEXT("component_name"), TEXT("Exact component name from component/list"))
        .RequiredString(TEXT("property_name"),  TEXT("Exact reflected property name"))
        .RequiredString(TEXT("value"),          TEXT("Value in UE ImportText form matching the property type"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("attach"),
            TEXT("Attach one scene component to another scene component on the SAME actor (AttachToComponent). Returns {attached (bool), child, parent}. "
                 "Params: actor_name (string, required, actor LABEL), child_component (string, required), parent_component (string, required), "
                 "location_rule / rotation_rule / scale_rule (enum KeepRelative|KeepWorld|SnapToTarget, optional, each default KeepRelative), "
                 "socket (string, optional, named socket on the parent; omit to attach to the parent origin). "
                 "Workflow: run component/list and pick two entries with is_scene_component=true before calling. "
                 "Warning: both components must be USceneComponent subclasses on the same actor or it errors. Always check the returned attached flag -- AttachToComponent can return false (e.g. attaching to a descendant would create a cycle)."))
        .RequiredString(TEXT("actor_name"),       TEXT("Actor label as shown in the World Outliner"))
        .RequiredString(TEXT("child_component"),  TEXT("Child scene component name (must be a USceneComponent)"))
        .RequiredString(TEXT("parent_component"), TEXT("Parent scene component name (must be a USceneComponent)"))
        .OptionalEnum  (TEXT("location_rule"),    {TEXT("KeepRelative"), TEXT("KeepWorld"), TEXT("SnapToTarget")}, TEXT("Location attachment rule (default KeepRelative)"))
        .OptionalEnum  (TEXT("rotation_rule"),    {TEXT("KeepRelative"), TEXT("KeepWorld"), TEXT("SnapToTarget")}, TEXT("Rotation attachment rule (default KeepRelative)"))
        .OptionalEnum  (TEXT("scale_rule"),       {TEXT("KeepRelative"), TEXT("KeepWorld"), TEXT("SnapToTarget")}, TEXT("Scale attachment rule (default KeepRelative)"))
        .OptionalString(TEXT("socket"),           TEXT("Optional named socket on the parent component"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("detach"),
            TEXT("Detach a scene component from its current parent (DetachFromComponent). Returns {child}. "
                 "Params: actor_name (string, required, actor LABEL), child_component (string, required, scene component to detach), "
                 "location_rule / rotation_rule / scale_rule (enum KeepRelative|KeepWorld, optional, each default KeepWorld so the component keeps its world transform). "
                 "Workflow: inverse of component/attach; use KeepWorld (default) to leave the component visually in place. "
                 "Warning: the child must be a USceneComponent or it errors; calling on a component with no parent is effectively a no-op. Does not save the level."))
        .RequiredString(TEXT("actor_name"),      TEXT("Actor label as shown in the World Outliner"))
        .RequiredString(TEXT("child_component"), TEXT("Child scene component name to detach"))
        .OptionalEnum  (TEXT("location_rule"),   {TEXT("KeepRelative"), TEXT("KeepWorld")}, TEXT("Location detachment rule (default KeepWorld)"))
        .OptionalEnum  (TEXT("rotation_rule"),   {TEXT("KeepRelative"), TEXT("KeepWorld")}, TEXT("Rotation detachment rule (default KeepWorld)"))
        .OptionalEnum  (TEXT("scale_rule"),      {TEXT("KeepRelative"), TEXT("KeepWorld")}, TEXT("Scale detachment rule (default KeepWorld)"))
        .Build());

    return Tools;
}

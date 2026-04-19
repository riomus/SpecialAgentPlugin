// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ComponentService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

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

FMCPResponse FComponentService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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

        Actor->RemoveInstanceComponent(Comp);
        Comp->DestroyComponent();

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

        Comp->Modify();
        void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
        const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Comp, PPF_None);
        if (ImportResult == nullptr)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("ImportText_Direct failed for '%s' on property '%s'"), *Value, *PropertyName));
        }

        FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
        Comp->PostEditChangeProperty(ChangeEvent);

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

        const bool bAttached = Child->AttachToComponent(Parent, Rules, SocketName.IsEmpty() ? NAME_None : FName(*SocketName));

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

        Child->DetachFromComponent(Rules);

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
            TEXT("Add a component to an actor. Instantiates via NewObject and registers at runtime. "
                 "Params: actor_name (string, actor label), component_class (string, native class name like 'StaticMeshComponent' or asset path to a blueprint component class), component_name (string, optional desired name). "
                 "Workflow: use world/list_actors to discover labels, then component/list to confirm. "
                 "Warning: treated as an instance component and lost on level reload unless the actor is re-saved."))
        .RequiredString(TEXT("actor_name"),      TEXT("Actor label"))
        .RequiredString(TEXT("component_class"), TEXT("Component class name or asset path"))
        .OptionalString(TEXT("component_name"),  TEXT("Optional name for the new component"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("remove"),
            TEXT("Remove a component from an actor. Calls DestroyComponent. "
                 "Params: actor_name (string), component_name (string, exact name as reported by component/list). "
                 "Workflow: use component/list to discover components. "
                 "Warning: irreversible; cannot remove the root component."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredString(TEXT("component_name"), TEXT("Component name"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("list"),
            TEXT("List all components on an actor. Returns name, class, and is_scene_component flag per entry. "
                 "Params: actor_name (string). "
                 "Workflow: precedes component/get_properties and component/set_property."))
        .RequiredString(TEXT("actor_name"), TEXT("Actor label"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("get_properties"),
            TEXT("Get all UProperty values from a component as strings (via TFieldIterator). "
                 "Params: actor_name (string), component_name (string). "
                 "Workflow: pair with component/set_property to round-trip edits. "
                 "Warning: values are stringified (ExportTextItem_Direct); struct payloads may be verbose."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredString(TEXT("component_name"), TEXT("Component name"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_property"),
            TEXT("Set a UProperty on a component by name via ImportText_Direct. Accepts any type the UE text importer understands. "
                 "Params: actor_name (string), component_name (string), property_name (string), value (string, UE text form e.g. '(X=1,Y=2,Z=3)' for FVector, 'true/false' for bool). "
                 "Workflow: call component/get_properties first to inspect the current stringified value. "
                 "Warning: malformed text is rejected but a correctly-parsed-but-wrong value still applies."))
        .RequiredString(TEXT("actor_name"),     TEXT("Actor label"))
        .RequiredString(TEXT("component_name"), TEXT("Component name"))
        .RequiredString(TEXT("property_name"),  TEXT("Property name"))
        .RequiredString(TEXT("value"),          TEXT("Value in UE text form"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("attach"),
            TEXT("Attach one scene component to another on the same actor via AttachToComponent. "
                 "Params: actor_name (string), child_component (string), parent_component (string), "
                 "location_rule/rotation_rule/scale_rule (enum KeepRelative|KeepWorld|SnapToTarget, default KeepRelative), "
                 "socket (string, optional socket name on parent). "
                 "Workflow: use component/list to enumerate scene components first. "
                 "Warning: both components must be USceneComponent subclasses."))
        .RequiredString(TEXT("actor_name"),       TEXT("Actor label"))
        .RequiredString(TEXT("child_component"),  TEXT("Child scene component name"))
        .RequiredString(TEXT("parent_component"), TEXT("Parent scene component name"))
        .OptionalEnum  (TEXT("location_rule"),    {TEXT("KeepRelative"), TEXT("KeepWorld"), TEXT("SnapToTarget")}, TEXT("Location attachment rule"))
        .OptionalEnum  (TEXT("rotation_rule"),    {TEXT("KeepRelative"), TEXT("KeepWorld"), TEXT("SnapToTarget")}, TEXT("Rotation attachment rule"))
        .OptionalEnum  (TEXT("scale_rule"),       {TEXT("KeepRelative"), TEXT("KeepWorld"), TEXT("SnapToTarget")}, TEXT("Scale attachment rule"))
        .OptionalString(TEXT("socket"),           TEXT("Optional socket name on the parent"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("detach"),
            TEXT("Detach a scene component from its parent via DetachFromComponent. "
                 "Params: actor_name (string), child_component (string), "
                 "location_rule/rotation_rule/scale_rule (enum KeepRelative|KeepWorld, default KeepWorld). "
                 "Workflow: inverse of component/attach. "
                 "Warning: no-op if the component has no parent."))
        .RequiredString(TEXT("actor_name"),      TEXT("Actor label"))
        .RequiredString(TEXT("child_component"), TEXT("Child scene component name"))
        .OptionalEnum  (TEXT("location_rule"),   {TEXT("KeepRelative"), TEXT("KeepWorld")}, TEXT("Location detachment rule"))
        .OptionalEnum  (TEXT("rotation_rule"),   {TEXT("KeepRelative"), TEXT("KeepWorld")}, TEXT("Rotation detachment rule"))
        .OptionalEnum  (TEXT("scale_rule"),      {TEXT("KeepRelative"), TEXT("KeepWorld")}, TEXT("Scale detachment rule"))
        .Build());

    return Tools;
}

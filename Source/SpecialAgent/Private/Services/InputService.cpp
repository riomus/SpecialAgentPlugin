// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/InputService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"

namespace
{
    TSharedPtr<FJsonObject> SerializeActionMapping(const FInputActionKeyMapping& M)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("action_name"), M.ActionName.ToString());
        O->SetStringField(TEXT("key"),         M.Key.ToString());
        O->SetBoolField  (TEXT("shift"),       M.bShift != 0);
        O->SetBoolField  (TEXT("ctrl"),        M.bCtrl  != 0);
        O->SetBoolField  (TEXT("alt"),         M.bAlt   != 0);
        O->SetBoolField  (TEXT("cmd"),         M.bCmd   != 0);
        return O;
    }

    TSharedPtr<FJsonObject> SerializeAxisMapping(const FInputAxisKeyMapping& M)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("axis_name"), M.AxisName.ToString());
        O->SetStringField(TEXT("key"),       M.Key.ToString());
        O->SetNumberField(TEXT("scale"),     M.Scale);
        return O;
    }
}

FString FInputService::GetServiceDescription() const
{
    return TEXT("Input mapping query and edit (legacy UInputSettings)");
}

FMCPResponse FInputService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    if (MethodName == TEXT("list_mappings"))       return HandleListMappings(Request);
    if (MethodName == TEXT("add_action_mapping"))  return HandleAddActionMapping(Request);
    if (MethodName == TEXT("add_axis_mapping"))    return HandleAddAxisMapping(Request);
    if (MethodName == TEXT("remove_mapping"))      return HandleRemoveMapping(Request);

    return MethodNotFound(Request.Id, TEXT("input"), MethodName);
}

FMCPResponse FInputService::HandleListMappings(const FMCPRequest& Request)
{
    FString Filter;
    if (Request.Params.IsValid())
    {
        FMCPJson::ReadString(Request.Params, TEXT("filter"), Filter);
    }

    auto Task = [Filter]() -> TSharedPtr<FJsonObject>
    {
        UInputSettings* Settings = UInputSettings::GetInputSettings();
        if (!Settings)
        {
            return FMCPJson::MakeError(TEXT("UInputSettings unavailable"));
        }

        TArray<TSharedPtr<FJsonValue>> ActionJson;
        for (const FInputActionKeyMapping& M : Settings->GetActionMappings())
        {
            if (!Filter.IsEmpty() && !M.ActionName.ToString().Contains(Filter)) continue;
            ActionJson.Add(MakeShared<FJsonValueObject>(SerializeActionMapping(M)));
        }

        TArray<TSharedPtr<FJsonValue>> AxisJson;
        for (const FInputAxisKeyMapping& M : Settings->GetAxisMappings())
        {
            if (!Filter.IsEmpty() && !M.AxisName.ToString().Contains(Filter)) continue;
            AxisJson.Add(MakeShared<FJsonValueObject>(SerializeAxisMapping(M)));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetArrayField (TEXT("actions"),      ActionJson);
        Result->SetArrayField (TEXT("axes"),         AxisJson);
        Result->SetNumberField(TEXT("action_count"), ActionJson.Num());
        Result->SetNumberField(TEXT("axis_count"),   AxisJson.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Listed %d action + %d axis mappings (filter=\"%s\")"),
            ActionJson.Num(), AxisJson.Num(), *Filter);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleAddActionMapping(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ActionName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("action_name"), ActionName) || ActionName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'action_name'"));

    FString KeyName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("key"), KeyName) || KeyName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'key' (e.g. SpaceBar, LeftMouseButton)"));

    bool bShift = false, bCtrl = false, bAlt = false, bCmd = false;
    FMCPJson::ReadBool(Request.Params, TEXT("shift"), bShift);
    FMCPJson::ReadBool(Request.Params, TEXT("ctrl"),  bCtrl);
    FMCPJson::ReadBool(Request.Params, TEXT("alt"),   bAlt);
    FMCPJson::ReadBool(Request.Params, TEXT("cmd"),   bCmd);

    bool bSave = true;
    FMCPJson::ReadBool(Request.Params, TEXT("save"), bSave);

    auto Task = [ActionName, KeyName, bShift, bCtrl, bAlt, bCmd, bSave]() -> TSharedPtr<FJsonObject>
    {
        UInputSettings* Settings = UInputSettings::GetInputSettings();
        if (!Settings)
        {
            return FMCPJson::MakeError(TEXT("UInputSettings unavailable"));
        }

        const FKey Key(*KeyName);
        if (!Key.IsValid())
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Invalid key name: %s (see EKeys list, e.g. SpaceBar, W, LeftMouseButton)"), *KeyName));
        }

        FInputActionKeyMapping Mapping(FName(*ActionName), Key, bShift, bCtrl, bAlt, bCmd);
        Settings->AddActionMapping(Mapping, /*bForceRebuildKeymaps=*/true);
        if (bSave)
        {
            Settings->SaveKeyMappings();
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetObjectField(TEXT("mapping"), SerializeActionMapping(Mapping));
        Result->SetBoolField  (TEXT("saved"),   bSave);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Added action mapping '%s' -> %s (save=%s)"),
            *ActionName, *KeyName, bSave ? TEXT("true") : TEXT("false"));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleAddAxisMapping(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString AxisName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("axis_name"), AxisName) || AxisName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'axis_name'"));

    FString KeyName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("key"), KeyName) || KeyName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'key'"));

    double Scale = 1.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("scale"), Scale);

    bool bSave = true;
    FMCPJson::ReadBool(Request.Params, TEXT("save"), bSave);

    auto Task = [AxisName, KeyName, Scale, bSave]() -> TSharedPtr<FJsonObject>
    {
        UInputSettings* Settings = UInputSettings::GetInputSettings();
        if (!Settings)
        {
            return FMCPJson::MakeError(TEXT("UInputSettings unavailable"));
        }

        const FKey Key(*KeyName);
        if (!Key.IsValid())
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Invalid key name: %s"), *KeyName));
        }

        FInputAxisKeyMapping Mapping(FName(*AxisName), Key, static_cast<float>(Scale));
        Settings->AddAxisMapping(Mapping, /*bForceRebuildKeymaps=*/true);
        if (bSave)
        {
            Settings->SaveKeyMappings();
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetObjectField(TEXT("mapping"), SerializeAxisMapping(Mapping));
        Result->SetBoolField  (TEXT("saved"),   bSave);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Added axis mapping '%s' -> %s (scale=%.2f, save=%s)"),
            *AxisName, *KeyName, static_cast<float>(Scale), bSave ? TEXT("true") : TEXT("false"));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleRemoveMapping(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString MappingType;
    if (!FMCPJson::ReadString(Request.Params, TEXT("mapping_type"), MappingType) || MappingType.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'mapping_type' ('action' or 'axis')"));

    MappingType = MappingType.ToLower();
    if (MappingType != TEXT("action") && MappingType != TEXT("axis"))
        return InvalidParams(Request.Id, TEXT("'mapping_type' must be 'action' or 'axis'"));

    FString Name;
    if (!FMCPJson::ReadString(Request.Params, TEXT("name"), Name) || Name.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'name' (action/axis name)"));

    // Optional: key to scope the removal to a single binding. Empty = remove all bindings for that name.
    FString KeyName;
    FMCPJson::ReadString(Request.Params, TEXT("key"), KeyName);

    bool bSave = true;
    FMCPJson::ReadBool(Request.Params, TEXT("save"), bSave);

    auto Task = [MappingType, Name, KeyName, bSave]() -> TSharedPtr<FJsonObject>
    {
        UInputSettings* Settings = UInputSettings::GetInputSettings();
        if (!Settings)
        {
            return FMCPJson::MakeError(TEXT("UInputSettings unavailable"));
        }

        const FName NameFN(*Name);
        const FKey ScopedKey = KeyName.IsEmpty() ? FKey() : FKey(*KeyName);
        if (!KeyName.IsEmpty() && !ScopedKey.IsValid())
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("Invalid key name: %s"), *KeyName));
        }

        int32 RemovedCount = 0;

        if (MappingType == TEXT("action"))
        {
            // Snapshot to avoid iterator invalidation while removing.
            TArray<FInputActionKeyMapping> All = Settings->GetActionMappings();
            for (const FInputActionKeyMapping& M : All)
            {
                if (M.ActionName != NameFN) continue;
                if (!KeyName.IsEmpty() && M.Key != ScopedKey) continue;
                Settings->RemoveActionMapping(M, /*bForceRebuildKeymaps=*/false);
                ++RemovedCount;
            }
        }
        else
        {
            TArray<FInputAxisKeyMapping> All = Settings->GetAxisMappings();
            for (const FInputAxisKeyMapping& M : All)
            {
                if (M.AxisName != NameFN) continue;
                if (!KeyName.IsEmpty() && M.Key != ScopedKey) continue;
                Settings->RemoveAxisMapping(M, /*bForceRebuildKeymaps=*/false);
                ++RemovedCount;
            }
        }

        Settings->ForceRebuildKeymaps();
        if (bSave)
        {
            Settings->SaveKeyMappings();
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("mapping_type"), MappingType);
        Result->SetStringField(TEXT("name"),         Name);
        Result->SetStringField(TEXT("key_filter"),   KeyName);
        Result->SetNumberField(TEXT("removed"),      RemovedCount);
        Result->SetBoolField  (TEXT("saved"),        bSave);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Removed %d %s mapping(s) for '%s' (key=%s)"),
            RemovedCount, *MappingType, *Name, KeyName.IsEmpty() ? TEXT("*") : *KeyName);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FInputService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("list_mappings"),
            TEXT("List all legacy action and axis mappings from UInputSettings::GetInputSettings(). "
                 "Params: filter (string, optional substring match on action/axis name). "
                 "Workflow: call before input/add_action_mapping to see existing bindings. "
                 "Warning: only covers legacy input; Enhanced Input (UInputMappingContext) is not queried here."))
        .OptionalString(TEXT("filter"), TEXT("Optional substring match on action_name / axis_name"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("add_action_mapping"),
            TEXT("Add a legacy action mapping (name+key+modifiers). Runs ForceRebuildKeymaps and optionally SaveKeyMappings. "
                 "Params: action_name (string), key (string, EKeys name e.g. SpaceBar), shift/ctrl/alt/cmd (bool, default false), save (bool, default true). "
                 "Workflow: pairs with input/list_mappings to verify. "
                 "Warning: duplicates with identical key+modifiers are rejected by UInputSettings."))
        .RequiredString(TEXT("action_name"), TEXT("Action name (e.g. Jump, Fire)"))
        .RequiredString(TEXT("key"),         TEXT("Key name (EKeys::, e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom)"))
        .OptionalBool  (TEXT("shift"),       TEXT("Require Shift modifier"))
        .OptionalBool  (TEXT("ctrl"),        TEXT("Require Ctrl modifier"))
        .OptionalBool  (TEXT("alt"),         TEXT("Require Alt modifier"))
        .OptionalBool  (TEXT("cmd"),         TEXT("Require Cmd modifier"))
        .OptionalBool  (TEXT("save"),        TEXT("Persist via SaveKeyMappings() (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("add_axis_mapping"),
            TEXT("Add a legacy axis mapping (name+key+scale). Runs ForceRebuildKeymaps and optionally SaveKeyMappings. "
                 "Params: axis_name (string), key (string, EKeys name), scale (number, default 1.0), save (bool, default true). "
                 "Workflow: pairs with input/list_mappings to verify. "
                 "Warning: scale typically -1/0/1 for digital keys; analog sticks should use their own axis Key (e.g. Gamepad_LeftX)."))
        .RequiredString(TEXT("axis_name"), TEXT("Axis name (e.g. MoveForward, LookRight)"))
        .RequiredString(TEXT("key"),       TEXT("Key name (EKeys::, e.g. W, Gamepad_LeftX)"))
        .OptionalNumber(TEXT("scale"),     TEXT("Scale multiplier applied to the axis input (default 1.0)"))
        .OptionalBool  (TEXT("save"),      TEXT("Persist via SaveKeyMappings() (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("remove_mapping"),
            TEXT("Remove action or axis mapping(s) by name, optionally scoped by key. "
                 "Params: mapping_type (enum action|axis), name (string), key (string, optional — empty removes all with that name), save (bool, default true). "
                 "Workflow: call input/list_mappings to confirm the removal. "
                 "Warning: removing by name without 'key' removes every binding for that action/axis."))
        .RequiredEnum  (TEXT("mapping_type"), { TEXT("action"), TEXT("axis") },
                                               TEXT("Which mapping table to edit"))
        .RequiredString(TEXT("name"),         TEXT("Action or axis name to remove"))
        .OptionalString(TEXT("key"),          TEXT("Restrict removal to this key; empty = remove all bindings for 'name'"))
        .OptionalBool  (TEXT("save"),         TEXT("Persist via SaveKeyMappings() (default true)"))
        .Build());

    return Tools;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/InputService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"

// Enhanced Input
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "EditorAssetLibrary.h"

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

    FString InputActionValueTypeToString(EInputActionValueType Type)
    {
        switch (Type)
        {
        case EInputActionValueType::Boolean: return TEXT("Bool");
        case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
        case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
        case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
        }
        return TEXT("Unknown");
    }

    TSharedPtr<FJsonObject> SerializeEnhancedMapping(const FEnhancedActionKeyMapping& M)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("action_path"),
            M.Action ? M.Action->GetPathName() : FString());
        O->SetStringField(TEXT("key"), M.Key.ToString());

        TArray<TSharedPtr<FJsonValue>> ModifierJson;
        for (const TObjectPtr<UInputModifier>& Mod : M.Modifiers)
        {
            if (Mod)
            {
                ModifierJson.Add(MakeShared<FJsonValueString>(Mod->GetClass()->GetName()));
            }
        }
        O->SetArrayField(TEXT("modifiers"), ModifierJson);

        TArray<TSharedPtr<FJsonValue>> TriggerJson;
        for (const TObjectPtr<UInputTrigger>& Trig : M.Triggers)
        {
            if (Trig)
            {
                TriggerJson.Add(MakeShared<FJsonValueString>(Trig->GetClass()->GetName()));
            }
        }
        O->SetArrayField(TEXT("triggers"), TriggerJson);
        return O;
    }
}

FString FInputService::GetServiceDescription() const
{
    return TEXT("Input mapping query and edit (legacy UInputSettings + Enhanced Input)");
}

FMCPResponse FInputService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    // Legacy UInputSettings
    if (MethodName == TEXT("list_mappings"))          return HandleListMappings(Request);
    if (MethodName == TEXT("add_action_mapping"))     return HandleAddActionMapping(Request);
    if (MethodName == TEXT("add_axis_mapping"))       return HandleAddAxisMapping(Request);
    if (MethodName == TEXT("remove_mapping"))         return HandleRemoveMapping(Request);

    // Enhanced Input
    if (MethodName == TEXT("list_enhanced_actions"))  return HandleListEnhancedActions(Request);
    if (MethodName == TEXT("list_mapping_contexts"))  return HandleListMappingContexts(Request);
    if (MethodName == TEXT("get_mapping_context"))    return HandleGetMappingContext(Request);
    if (MethodName == TEXT("add_enhanced_mapping"))   return HandleAddEnhancedMapping(Request);
    if (MethodName == TEXT("remove_enhanced_mapping"))return HandleRemoveEnhancedMapping(Request);

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

// ---------------------------------------------------------------------------
// Enhanced Input handlers
// ---------------------------------------------------------------------------

FMCPResponse FInputService::HandleListEnhancedActions(const FMCPRequest& Request)
{
    FString PathFilter = TEXT("/Game");
    int32 MaxResults = 1000;
    if (Request.Params.IsValid())
    {
        FMCPJson::ReadString (Request.Params, TEXT("path"),        PathFilter);
        FMCPJson::ReadInteger(Request.Params, TEXT("max_results"), MaxResults);
    }

    auto Task = [PathFilter, MaxResults]() -> TSharedPtr<FJsonObject>
    {
        IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
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
            Obj->SetStringField(TEXT("path"), A.GetObjectPathString());

            // Prefer cheap asset-registry tag lookup for ValueType; fall back to load.
            FString ValueTypeStr;
            if (!A.GetTagValue(GET_MEMBER_NAME_CHECKED(UInputAction, ValueType), ValueTypeStr) || ValueTypeStr.IsEmpty())
            {
                if (const UInputAction* Action = Cast<UInputAction>(A.GetAsset()))
                {
                    ValueTypeStr = InputActionValueTypeToString(Action->ValueType);
                }
                else
                {
                    ValueTypeStr = TEXT("Unknown");
                }
            }
            else
            {
                // Asset registry stores the raw enum identifier (e.g. "Boolean"/"Axis1D").
                if (ValueTypeStr == TEXT("Boolean")) ValueTypeStr = TEXT("Bool");
            }
            Obj->SetStringField(TEXT("value_type"), ValueTypeStr);
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetArrayField (TEXT("actions"), Out);
        Result->SetNumberField(TEXT("count"),   Out.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: input/list_enhanced_actions -> %d entries (path=%s)"),
            Out.Num(), *PathFilter);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleListMappingContexts(const FMCPRequest& Request)
{
    FString PathFilter = TEXT("/Game");
    int32 MaxResults = 1000;
    if (Request.Params.IsValid())
    {
        FMCPJson::ReadString (Request.Params, TEXT("path"),        PathFilter);
        FMCPJson::ReadInteger(Request.Params, TEXT("max_results"), MaxResults);
    }

    auto Task = [PathFilter, MaxResults]() -> TSharedPtr<FJsonObject>
    {
        IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
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
            Obj->SetStringField(TEXT("path"), A.GetObjectPathString());

            int32 MappingCount = 0;
            if (const UInputMappingContext* Context = Cast<UInputMappingContext>(A.GetAsset()))
            {
                MappingCount = Context->GetMappings().Num();
            }
            Obj->SetNumberField(TEXT("mapping_count"), MappingCount);
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetArrayField (TEXT("contexts"), Out);
        Result->SetNumberField(TEXT("count"),    Out.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: input/list_mapping_contexts -> %d entries (path=%s)"),
            Out.Num(), *PathFilter);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleGetMappingContext(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ContextPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("context_path"), ContextPath) || ContextPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'context_path' (e.g. /Game/Input/IMC_Default.IMC_Default)"));

    auto Task = [ContextPath]() -> TSharedPtr<FJsonObject>
    {
        UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
        if (!Context)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("UInputMappingContext not found at '%s'"), *ContextPath));
        }

        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FEnhancedActionKeyMapping& M : Context->GetMappings())
        {
            Out.Add(MakeShared<FJsonValueObject>(SerializeEnhancedMapping(M)));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("context_path"), ContextPath);
        Result->SetArrayField (TEXT("mappings"),     Out);
        Result->SetNumberField(TEXT("count"),        Out.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: input/get_mapping_context '%s' -> %d mappings"),
            *ContextPath, Out.Num());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleAddEnhancedMapping(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ContextPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("context_path"), ContextPath) || ContextPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'context_path'"));

    FString ActionPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'action_path'"));

    FString KeyName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("key"), KeyName) || KeyName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'key' (e.g. SpaceBar, W, Gamepad_FaceButton_Bottom)"));

    bool bSave = true;
    FMCPJson::ReadBool(Request.Params, TEXT("save"), bSave);

    auto Task = [ContextPath, ActionPath, KeyName, bSave]() -> TSharedPtr<FJsonObject>
    {
        UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
        if (!Context)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("UInputMappingContext not found at '%s'"), *ContextPath));
        }

        UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
        if (!Action)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("UInputAction not found at '%s'"), *ActionPath));
        }

        const FKey Key{FName(*KeyName)};
        if (!Key.IsValid())
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Invalid key name: %s (see EKeys list, e.g. SpaceBar, W, LeftMouseButton)"), *KeyName));
        }

        FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
        Context->MarkPackageDirty();

        bool bSaved = false;
        if (bSave)
        {
            bSaved = UEditorAssetLibrary::SaveAsset(ContextPath, /*bOnlyIfDirty=*/false);
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("context_path"), ContextPath);
        Result->SetObjectField(TEXT("mapping"),      SerializeEnhancedMapping(Mapping));
        Result->SetBoolField  (TEXT("saved"),        bSaved);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: input/add_enhanced_mapping '%s' + '%s' -> %s (saved=%s)"),
            *ContextPath, *ActionPath, *KeyName, bSaved ? TEXT("true") : TEXT("false"));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FInputService::HandleRemoveEnhancedMapping(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString ContextPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("context_path"), ContextPath) || ContextPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'context_path'"));

    FString ActionPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'action_path'"));

    FString KeyName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("key"), KeyName) || KeyName.IsEmpty())
        return InvalidParams(Request.Id, TEXT("Missing 'key'"));

    bool bSave = true;
    FMCPJson::ReadBool(Request.Params, TEXT("save"), bSave);

    auto Task = [ContextPath, ActionPath, KeyName, bSave]() -> TSharedPtr<FJsonObject>
    {
        UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
        if (!Context)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("UInputMappingContext not found at '%s'"), *ContextPath));
        }

        UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
        if (!Action)
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("UInputAction not found at '%s'"), *ActionPath));
        }

        const FKey Key{FName(*KeyName)};
        if (!Key.IsValid())
        {
            return FMCPJson::MakeError(FString::Printf(
                TEXT("Invalid key name: %s"), *KeyName));
        }

        const int32 BeforeCount = Context->GetMappings().Num();
        Context->UnmapKey(Action, Key);
        const int32 AfterCount = Context->GetMappings().Num();
        const int32 RemovedCount = BeforeCount - AfterCount;

        Context->MarkPackageDirty();

        bool bSaved = false;
        if (bSave)
        {
            bSaved = UEditorAssetLibrary::SaveAsset(ContextPath, /*bOnlyIfDirty=*/false);
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("context_path"), ContextPath);
        Result->SetStringField(TEXT("action_path"),  ActionPath);
        Result->SetStringField(TEXT("key"),          KeyName);
        Result->SetNumberField(TEXT("removed"),      RemovedCount);
        Result->SetBoolField  (TEXT("saved"),        bSaved);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: input/remove_enhanced_mapping '%s' - '%s' @ %s -> removed=%d (saved=%s)"),
            *ContextPath, *ActionPath, *KeyName, RemovedCount, bSaved ? TEXT("true") : TEXT("false"));
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
            TEXT("List legacy UInputSettings action and axis mappings. Returns {success, actions:[{action_name, key, shift, ctrl, alt, cmd}], axes:[{axis_name, key, scale}], action_count, axis_count}. "
                 "Params: filter (string, optional — case-insensitive substring match on action_name / axis_name; empty returns all). "
                 "Workflow: call before input/add_action_mapping or input/remove_mapping to see existing bindings by name+key. "
                 "Warning: read-only. Covers ONLY the legacy input system, which is deprecated since 5.1 (read/migrate only) — Enhanced Input is the default; use input/list_enhanced_actions and input/list_mapping_contexts for that."))
        .OptionalString(TEXT("filter"), TEXT("Case-sensitive substring match on action_name / axis_name (optional; empty returns all)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("add_action_mapping"),
            TEXT("Add a legacy UInputSettings action mapping (name + key + modifiers). Returns {success, mapping:{action_name, key, shift, ctrl, alt, cmd}, saved}. "
                 "Params: action_name (string, required), key (string, required — an EKeys name e.g. SpaceBar / W / LeftMouseButton / Gamepad_FaceButton_Bottom), shift/ctrl/alt/cmd (bool, optional, default false), save (bool, optional, default true — persist via SaveKeyMappings). "
                 "Workflow: call input/list_mappings afterward to verify; rebuilds keymaps immediately. "
                 "Warning: legacy input is deprecated since 5.1 — prefer Enhanced Input (input/add_enhanced_mapping). AddActionMapping does NOT dedupe: repeating the call appends a duplicate binding. Invalid key names return error 'Invalid key name: <key>'."))
        .RequiredString(TEXT("action_name"), TEXT("Action name (e.g. Jump, Fire)"))
        .RequiredString(TEXT("key"),         TEXT("EKeys name (e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom)"))
        .OptionalBool  (TEXT("shift"),       TEXT("Require Shift modifier (default false)"))
        .OptionalBool  (TEXT("ctrl"),        TEXT("Require Ctrl modifier (default false)"))
        .OptionalBool  (TEXT("alt"),         TEXT("Require Alt modifier (default false)"))
        .OptionalBool  (TEXT("cmd"),         TEXT("Require Cmd modifier (default false)"))
        .OptionalBool  (TEXT("save"),        TEXT("Persist via SaveKeyMappings (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("add_axis_mapping"),
            TEXT("Add a legacy UInputSettings axis mapping (name + key + scale). Returns {success, mapping:{axis_name, key, scale}, saved}. "
                 "Params: axis_name (string, required), key (string, required — an EKeys name e.g. W / Gamepad_LeftX), scale (number, optional, default 1.0 — multiplier applied to the input), save (bool, optional, default true — persist via SaveKeyMappings). "
                 "Workflow: call input/list_mappings afterward to verify; rebuilds keymaps immediately. "
                 "Warning: legacy input is deprecated since 5.1 — prefer Enhanced Input. scale is typically -1/0/1 for digital keys; analog sticks should bind their own axis Key (e.g. Gamepad_LeftX) with scale 1.0. Invalid key names return error 'Invalid key name: <key>'."))
        .RequiredString(TEXT("axis_name"), TEXT("Axis name (e.g. MoveForward, LookRight)"))
        .RequiredString(TEXT("key"),       TEXT("EKeys name (e.g. W, Gamepad_LeftX)"))
        .OptionalNumber(TEXT("scale"),     TEXT("Scale multiplier applied to the axis input (default 1.0)"))
        .OptionalBool  (TEXT("save"),      TEXT("Persist via SaveKeyMappings (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("remove_mapping"),
            TEXT("Remove legacy UInputSettings action or axis mapping(s) by name, optionally scoped to a single key. Returns {success, mapping_type, name, key_filter, removed (count), saved}. "
                 "Params: mapping_type (enum 'action'|'axis', required, case-insensitive), name (string, required — action/axis name), key (string, optional — EKeys name; empty removes ALL bindings for that name), save (bool, optional, default true). "
                 "Workflow: call input/list_mappings first to see what exists, then again to confirm removal. "
                 "Warning: legacy input is deprecated since 5.1 — prefer Enhanced Input. Omitting 'key' removes every binding for that action/axis. Invalid key returns error 'Invalid key name: <key>'."))
        .RequiredEnum  (TEXT("mapping_type"), { TEXT("action"), TEXT("axis") },
                                               TEXT("Which mapping table to edit: 'action' or 'axis'"))
        .RequiredString(TEXT("name"),         TEXT("Action or axis name to remove"))
        .OptionalString(TEXT("key"),          TEXT("EKeys name to scope removal to; empty removes all bindings for 'name'"))
        .OptionalBool  (TEXT("save"),         TEXT("Persist via SaveKeyMappings (default true)"))
        .Build());

    // -----------------------------------------------------------------------
    // Enhanced Input (UInputAction / UInputMappingContext assets)
    // -----------------------------------------------------------------------

    Tools.Add(FMCPToolBuilder(
            TEXT("list_enhanced_actions"),
            TEXT("List Enhanced Input UInputAction assets via the asset registry (recursive, cached metadata — no asset load unless value_type is missing). Returns {success, actions:[{path (object path), value_type (Bool|Axis1D|Axis2D|Axis3D|Unknown)}], count}. "
                 "Params: path (string, optional — package-path root to search recursively, default /Game; must be a virtual path like /Game/Input, not an OS path), max_results (integer, optional, default 1000; negative = unlimited). "
                 "Workflow: call before input/add_enhanced_mapping to pick a valid action_path from the returned 'path' field. "
                 "Warning: read-only. Returns Enhanced Input actions only; legacy UInputSettings mappings come from input/list_mappings instead."))
        .OptionalString (TEXT("path"),        TEXT("Package-path root to search recursively (virtual path, default /Game)"))
        .OptionalInteger(TEXT("max_results"), TEXT("Max assets to return; negative = unlimited (default 1000)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("list_mapping_contexts"),
            TEXT("List Enhanced Input UInputMappingContext (IMC) assets via the asset registry, with each one's mapping count. Returns {success, contexts:[{path (object path), mapping_count}], count}. "
                 "Params: path (string, optional — package-path root to search recursively, default /Game; virtual path, not an OS path), max_results (integer, optional, default 1000; negative = unlimited). "
                 "Workflow: pairs with input/get_mapping_context to inspect a single IMC's mappings, and input/add_enhanced_mapping to edit one. "
                 "Warning: read-only, but computing mapping_count LOADS each IMC asset — slow on very large content sets; narrow 'path' or lower max_results."))
        .OptionalString (TEXT("path"),        TEXT("Package-path root to search recursively (virtual path, default /Game)"))
        .OptionalInteger(TEXT("max_results"), TEXT("Max assets to return; negative = unlimited (default 1000)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("get_mapping_context"),
            TEXT("Read all key mappings from one UInputMappingContext asset. Returns {success, context_path, mappings:[{action_path, key, modifiers:[class names], triggers:[class names]}], count}. "
                 "Params: context_path (string, required — IMC object path, e.g. /Game/Input/IMC_Default.IMC_Default; package name plus .Asset). "
                 "Workflow: call after input/list_mapping_contexts; pairs with input/add_enhanced_mapping / input/remove_enhanced_mapping to confirm bindings. "
                 "Warning: read-only (loads the asset). Modifiers and triggers are reported by class name only — their per-instance property values are not serialized; returns error if no IMC exists at context_path."))
        .RequiredString(TEXT("context_path"), TEXT("Object path of the UInputMappingContext asset (e.g. /Game/Input/IMC_Default.IMC_Default)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("add_enhanced_mapping"),
            TEXT("Bind a key to a UInputAction inside a UInputMappingContext (Context->MapKey + MarkPackageDirty, then optional save). Returns {success, context_path, mapping:{action_path, key, modifiers, triggers}, saved}. "
                 "Params: context_path (string, required — IMC object path, e.g. /Game/Input/IMC.IMC), action_path (string, required — UInputAction object path), key (string, required — EKeys name e.g. SpaceBar / W / Gamepad_FaceButton_Bottom), save (bool, optional, default true — writes the IMC package to disk). "
                 "Workflow: pick action_path via input/list_enhanced_actions, then input/get_mapping_context to verify. "
                 "Warning: MUTATES and (by default) saves the IMC asset. MapKey does NOT dedupe — repeating adds another mapping for the same action+key. Context priority is NOT stored here; it is supplied at runtime when AddMappingContext is called (higher priority wins/consumes). Returns error if the IMC, the action, or the key is invalid."))
        .RequiredString(TEXT("context_path"), TEXT("Object path of the UInputMappingContext asset (e.g. /Game/Input/IMC.IMC)"))
        .RequiredString(TEXT("action_path"),  TEXT("Object path of the UInputAction asset"))
        .RequiredString(TEXT("key"),          TEXT("EKeys name (e.g. SpaceBar, W, Gamepad_FaceButton_Bottom)"))
        .OptionalBool  (TEXT("save"),         TEXT("Persist the IMC asset to disk (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("remove_enhanced_mapping"),
            TEXT("Unbind a key from a UInputAction inside a UInputMappingContext (Context->UnmapKey + MarkPackageDirty, then optional save). Returns {success, context_path, action_path, key, removed (count), saved}. "
                 "Params: context_path (string, required — IMC object path), action_path (string, required — UInputAction object path), key (string, required — EKeys name e.g. SpaceBar / W), save (bool, optional, default true — writes the IMC package to disk). "
                 "Workflow: call input/get_mapping_context first to confirm which bindings exist. "
                 "Warning: MUTATES and (by default) saves the IMC asset. UnmapKey removes EVERY mapping matching action+key (usually one, but duplicates are possible) — check the returned 'removed' count. Returns error if the IMC, the action, or the key is invalid."))
        .RequiredString(TEXT("context_path"), TEXT("Object path of the UInputMappingContext asset"))
        .RequiredString(TEXT("action_path"),  TEXT("Object path of the UInputAction asset"))
        .RequiredString(TEXT("key"),          TEXT("EKeys name (e.g. SpaceBar, W)"))
        .OptionalBool  (TEXT("save"),         TEXT("Persist the IMC asset to disk (default true)"))
        .Build());

    return Tools;
}

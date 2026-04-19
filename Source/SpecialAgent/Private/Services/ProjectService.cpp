// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ProjectService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"

FString FProjectService::GetServiceDescription() const
{
    return TEXT("Project settings and plugin enablement");
}

TArray<FMCPToolInfo> FProjectService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("get_setting"),
        TEXT("Read a string setting from DefaultGame.ini (GGameIni). "
             "Params: section (string, required, e.g. '/Script/EngineSettings.GameMapsSettings'); key (string, required). "
             "Workflow: pair with project/set_setting to round-trip. "
             "Warning: returns found=false if key missing."))
        .RequiredString(TEXT("section"), TEXT("INI section header (e.g. '/Script/EngineSettings.GameMapsSettings')."))
        .RequiredString(TEXT("key"), TEXT("INI key name."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_setting"),
        TEXT("Write a string setting to DefaultGame.ini and flush to disk. "
             "Params: section (string, required); key (string, required); value (string, required). "
             "Workflow: verify with project/get_setting. "
             "Warning: some settings are cached by subsystems and may require an editor restart."))
        .RequiredString(TEXT("section"), TEXT("INI section header."))
        .RequiredString(TEXT("key"), TEXT("INI key name."))
        .RequiredString(TEXT("value"), TEXT("New value (string)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_version"),
        TEXT("Get the current Unreal Engine version. Returns {version} as the string form of FEngineVersion::Current (e.g. '5.7.0-...'). "
             "Workflow: useful for guarding scripts that call version-specific APIs."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_plugins"),
        TEXT("List all discovered plugins with their enabled state. "
             "Workflow: feed plugin names to project/enable_plugin or project/disable_plugin."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("enable_plugin"),
        TEXT("Enable a plugin in the current project (.uproject). "
             "Params: name (string, required, plugin name, not path). "
             "Warning: requires an editor restart to take effect for most plugins."))
        .RequiredString(TEXT("name"), TEXT("Plugin name (matches IPlugin::GetName)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("disable_plugin"),
        TEXT("Disable a plugin in the current project (.uproject). "
             "Params: name (string, required). "
             "Warning: requires an editor restart to fully unload."))
        .RequiredString(TEXT("name"), TEXT("Plugin name."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_content_path"),
        TEXT("Get the absolute filesystem path of the project's Content/ directory. Returns {path}. "
             "Workflow: use to resolve /Game/... asset paths to on-disk locations for python/execute_file or Mount points."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_project_path"),
        TEXT("Get the absolute filesystem path of the active .uproject file. Returns {path}. "
             "Workflow: combine with assets/* tools when scripting out-of-editor tooling."))
        .Build());

    return Tools;
}

FMCPResponse FProjectService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    if (MethodName == TEXT("get_setting"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString Section, Key;
        if (!FMCPJson::ReadString(Request.Params, TEXT("section"), Section) || Section.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'section'"));
        }
        if (!FMCPJson::ReadString(Request.Params, TEXT("key"), Key) || Key.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'key'"));
        }

        if (!GConfig)
        {
            return FMCPResponse::Success(Request.Id, FMCPJson::MakeError(TEXT("GConfig unavailable")));
        }

        FString Value;
        const bool bFound = GConfig->GetString(*Section, *Key, Value, GGameIni);

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("section"), Section);
        Out->SetStringField(TEXT("key"), Key);
        Out->SetBoolField(TEXT("found"), bFound);
        Out->SetStringField(TEXT("value"), Value);
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("set_setting"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString Section, Key, Value;
        if (!FMCPJson::ReadString(Request.Params, TEXT("section"), Section) || Section.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'section'"));
        }
        if (!FMCPJson::ReadString(Request.Params, TEXT("key"), Key) || Key.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'key'"));
        }
        if (!FMCPJson::ReadString(Request.Params, TEXT("value"), Value))
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'value'"));
        }

        if (!GConfig)
        {
            return FMCPResponse::Success(Request.Id, FMCPJson::MakeError(TEXT("GConfig unavailable")));
        }

        GConfig->SetString(*Section, *Key, *Value, GGameIni);
        GConfig->Flush(false, GGameIni);

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("section"), Section);
        Out->SetStringField(TEXT("key"), Key);
        Out->SetStringField(TEXT("value"), Value);
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: ini set [%s] %s = %s"), *Section, *Key, *Value);
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("get_version"))
    {
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("version"), FEngineVersion::Current().ToString());
        Out->SetNumberField(TEXT("major"), FEngineVersion::Current().GetMajor());
        Out->SetNumberField(TEXT("minor"), FEngineVersion::Current().GetMinor());
        Out->SetNumberField(TEXT("patch"), FEngineVersion::Current().GetPatch());
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("list_plugins"))
    {
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        TArray<TSharedPtr<FJsonValue>> Arr;
        const TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetDiscoveredPlugins();
        for (const TSharedRef<IPlugin>& Plugin : Plugins)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"),          Plugin->GetName());
            Obj->SetStringField(TEXT("friendly_name"), Plugin->GetFriendlyName());
            Obj->SetBoolField  (TEXT("enabled"),       Plugin->IsEnabled());
            Obj->SetBoolField  (TEXT("mounted"),       Plugin->IsMounted());
            Obj->SetStringField(TEXT("base_dir"),      Plugin->GetBaseDir());
            Arr.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Out->SetArrayField(TEXT("plugins"), Arr);
        Out->SetNumberField(TEXT("count"), Arr.Num());
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("enable_plugin") || MethodName == TEXT("disable_plugin"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString PluginName;
        if (!FMCPJson::ReadString(Request.Params, TEXT("name"), PluginName) || PluginName.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'name'"));
        }

        const bool bEnable = (MethodName == TEXT("enable_plugin"));
        FText FailReason;
        const bool bOk = IProjectManager::Get().SetPluginEnabled(PluginName, bEnable, FailReason);

        TSharedPtr<FJsonObject> Out = bOk ? FMCPJson::MakeSuccess()
                                          : FMCPJson::MakeError(FailReason.ToString());
        Out->SetStringField(TEXT("name"), PluginName);
        Out->SetBoolField(TEXT("enable"), bEnable);
        if (bOk)
        {
            // Persist to .uproject.
            FText SaveFail;
            const bool bSaved = IProjectManager::Get().SaveCurrentProjectToDisk(SaveFail);
            Out->SetBoolField(TEXT("saved"), bSaved);
            if (!bSaved)
            {
                Out->SetStringField(TEXT("save_error"), SaveFail.ToString());
            }
            Out->SetStringField(TEXT("note"), TEXT("Changes take effect after an editor restart."));
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: SetPluginEnabled %s=%d saved=%d"),
                *PluginName, bEnable ? 1 : 0, bSaved ? 1 : 0);
        }
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("get_content_path"))
    {
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("content_path"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("get_project_path"))
    {
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
        Out->SetStringField(TEXT("project_dir"),  FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
        return FMCPResponse::Success(Request.Id, Out);
    }

    return MethodNotFound(Request.Id, TEXT("project"), MethodName);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ProjectService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
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
        TEXT("Read a single string config value from the game INI hierarchy (GGameIni, backed by DefaultGame.ini). "
             "Returns {success, section, key, found, value}. "
             "Params: section (string, required, INI section header e.g. '/Script/EngineSettings.GameMapsSettings'); key (string, required, INI key name). "
             "Workflow: pair with project/set_setting to round-trip a value. "
             "Warning: read-only; returns found=false (and empty value) when the key is absent rather than erroring."))
        .RequiredString(TEXT("section"), TEXT("INI section header (e.g. '/Script/EngineSettings.GameMapsSettings')."))
        .RequiredString(TEXT("key"), TEXT("INI key name."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_setting"),
        TEXT("Write a string config value into the game INI hierarchy and flush it to DefaultGame.ini on disk. "
             "Returns {success, section, key, value}. "
             "Params: section (string, required, INI section header); key (string, required, INI key name); value (string, required, empty string allowed). "
             "Workflow: verify the write with project/get_setting afterwards. "
             "Warning: persists to disk immediately; many settings are cached by subsystems at startup, so changes often require an editor restart to take effect."))
        .RequiredString(TEXT("section"), TEXT("INI section header."))
        .RequiredString(TEXT("key"), TEXT("INI key name."))
        .RequiredString(TEXT("value"), TEXT("New value (string; empty string allowed)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_version"),
        TEXT("Get the running Unreal Engine version. "
             "Returns {success, version, major, minor, patch} where version is the full FEngineVersion::Current string (e.g. '5.7.0-...') and major/minor/patch are integers. "
             "Params: (none). "
             "Workflow: check major/minor before calling version-specific APIs (this codebase targets 5.7). "
             "Warning: read-only; reports the editor build, not the project's target version."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_plugins"),
        TEXT("List every discovered plugin with its enabled and mounted state. "
             "Returns {success, count, plugins:[{name, friendly_name, enabled, mounted, base_dir}]} where name matches IPlugin::GetName. "
             "Params: (none). "
             "Workflow: feed a plugin's 'name' to project/enable_plugin or project/disable_plugin. "
             "Warning: read-only; lists discovered plugins on disk, which is a superset of those enabled in the .uproject."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("enable_plugin"),
        TEXT("Enable a plugin in the current project and persist the change to the .uproject file. "
             "Returns {success, name, enable:true, saved, note} (save_error present if writing the .uproject failed). "
             "Params: name (string, required, plugin name matching IPlugin::GetName, not a path). "
             "Workflow: call project/list_plugins first to get the exact 'name'. "
             "Warning: writes the .uproject to disk; most plugins do NOT load until the editor is restarted."))
        .RequiredString(TEXT("name"), TEXT("Plugin name (matches IPlugin::GetName)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("disable_plugin"),
        TEXT("Disable a plugin in the current project and persist the change to the .uproject file. "
             "Returns {success, name, enable:false, saved, note} (save_error present if writing the .uproject failed). "
             "Params: name (string, required, plugin name matching IPlugin::GetName, not a path). "
             "Workflow: call project/list_plugins first to get the exact 'name'. "
             "Warning: writes the .uproject to disk; the plugin is not fully unloaded until the editor is restarted."))
        .RequiredString(TEXT("name"), TEXT("Plugin name (matches IPlugin::GetName)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_content_path"),
        TEXT("Get the absolute OS filesystem path of the project's Content/ directory. "
             "Returns {success, content_path} (an absolute OS path, NOT a /Game/ virtual path). "
             "Params: (none). "
             "Workflow: use to map a virtual /Game/... asset path to an on-disk location for python/execute_file or mount points. "
             "Warning: read-only; the /Game/ mount maps to this folder, but the two path forms are not interchangeable."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_project_path"),
        TEXT("Get the absolute OS filesystem paths of the active .uproject file and its containing project directory. "
             "Returns {success, project_file, project_dir} (both absolute OS paths). "
             "Params: (none). "
             "Workflow: combine with project/get_content_path when scripting out-of-editor tooling. "
             "Warning: read-only; these are OS paths, not /Game/ virtual paths."))
        .Build());

    return Tools;
}

FMCPResponse FProjectService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
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

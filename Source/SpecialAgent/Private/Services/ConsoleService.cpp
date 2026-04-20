// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ConsoleService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDeviceNull.h"

FString FConsoleService::GetServiceDescription() const
{
    return TEXT("Execute console commands and manipulate CVars");
}

TArray<FMCPToolInfo> FConsoleService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("execute"),
        TEXT("Execute an Unreal console command against the editor world. "
             "Params: command (string, required, full command line, e.g. 'stat fps' or 'r.ScreenPercentage 75'). "
             "Workflow: cross-reference with console/list_commands for examples. "
             "Warning: commands run in the editor world; PIE-specific commands may be ignored."))
        .RequiredString(TEXT("command"), TEXT("Console command line to execute."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_commands"),
        TEXT("Return a curated list of commonly-used console commands and CVars. "
             "Workflow: inspect before calling console/execute or console/set_cvar."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_cvar"),
        TEXT("Set a console variable by name. "
             "Params: name (string, required, CVar name e.g. 'r.ScreenPercentage'); value (string, required, new value). "
             "Workflow: use console/get_cvar to read back; console/list_commands for candidates. "
             "Warning: fails if the CVar does not exist."))
        .RequiredString(TEXT("name"), TEXT("CVar name (e.g. r.ScreenPercentage)."))
        .RequiredString(TEXT("value"), TEXT("New CVar value as string."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_cvar"),
        TEXT("Read a console variable's current value. "
             "Params: name (string, required, CVar name). "
             "Returns: string, float, int and bool renderings. "
             "Warning: fails if the CVar does not exist."))
        .RequiredString(TEXT("name"), TEXT("CVar name (e.g. r.ScreenPercentage)."))
        .Build());

    return Tools;
}

FMCPResponse FConsoleService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    if (MethodName == TEXT("execute"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString Command;
        if (!FMCPJson::ReadString(Request.Params, TEXT("command"), Command) || Command.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'command'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [Command]() -> TSharedPtr<FJsonObject>
            {
                if (!GEngine)
                {
                    return FMCPJson::MakeError(TEXT("GEngine unavailable"));
                }
                UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
                FOutputDeviceNull Null;
                const bool bExecuted = GEngine->Exec(World, *Command, Null);

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("command"), Command);
                Out->SetBoolField(TEXT("executed"), bExecuted);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: console execute '%s' -> %d"), *Command, bExecuted ? 1 : 0);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("list_commands"))
    {
        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        TArray<TSharedPtr<FJsonValue>> Arr;
        auto Add = [&Arr](const TCHAR* Name, const TCHAR* Desc)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Name);
            Entry->SetStringField(TEXT("description"), Desc);
            Arr.Add(MakeShared<FJsonValueObject>(Entry));
        };

        Add(TEXT("stat fps"),              TEXT("Toggle FPS counter HUD."));
        Add(TEXT("stat unit"),             TEXT("Show frame/game/draw/GPU timings."));
        Add(TEXT("stat unitgraph"),        TEXT("Same as stat unit with a history graph."));
        Add(TEXT("stat scenerendering"),   TEXT("Scene rendering statistics."));
        Add(TEXT("stat gpu"),              TEXT("Per-pass GPU timings."));
        Add(TEXT("stat memory"),           TEXT("Memory usage summary."));
        Add(TEXT("show collision"),        TEXT("Toggle collision wireframe in the viewport."));
        Add(TEXT("show navigation"),       TEXT("Toggle navigation mesh display."));
        Add(TEXT("show bounds"),           TEXT("Toggle actor bounds display."));
        Add(TEXT("toggledebugcamera"),     TEXT("Detach camera for free debug flying."));
        Add(TEXT("r.ScreenPercentage"),    TEXT("Primary render resolution percentage (CVar)."));
        Add(TEXT("r.Nanite"),              TEXT("Enable/disable Nanite (CVar)."));
        Add(TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("GI method: 0=None, 1=Lumen, 2=ScreenSpace (CVar)."));
        Add(TEXT("r.ReflectionMethod"),    TEXT("Reflection method: 0=None, 1=Lumen, 2=ScreenSpace (CVar)."));
        Add(TEXT("r.Shadow.Virtual.Enable"), TEXT("Toggle Virtual Shadow Maps (CVar)."));
        Add(TEXT("t.MaxFPS"),              TEXT("Frame-rate cap (CVar)."));
        Add(TEXT("slomo"),                 TEXT("Global world time dilation."));
        Add(TEXT("obj list"),              TEXT("Dump object list to log."));
        Add(TEXT("memreport -full"),       TEXT("Full memory report to log."));
        Add(TEXT("viewmode unlit"),        TEXT("Switch viewport to unlit view mode."));
        Add(TEXT("viewmode lit"),          TEXT("Switch viewport back to lit view mode."));
        Add(TEXT("viewmode wireframe"),    TEXT("Switch viewport to wireframe."));

        Out->SetArrayField(TEXT("commands"), Arr);
        return FMCPResponse::Success(Request.Id, Out);
    }

    if (MethodName == TEXT("set_cvar"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString Name, Value;
        if (!FMCPJson::ReadString(Request.Params, TEXT("name"), Name) || Name.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'name'"));
        }
        if (!FMCPJson::ReadString(Request.Params, TEXT("value"), Value))
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'value'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [Name, Value]() -> TSharedPtr<FJsonObject>
            {
                IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
                if (!CVar)
                {
                    return FMCPJson::MakeError(FString::Printf(TEXT("CVar '%s' not found"), *Name));
                }
                CVar->Set(*Value, ECVF_SetByConsole);

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("name"), Name);
                Out->SetStringField(TEXT("value"), CVar->GetString());
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: cvar set %s = %s"), *Name, *Value);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_cvar"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString Name;
        if (!FMCPJson::ReadString(Request.Params, TEXT("name"), Name) || Name.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'name'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [Name]() -> TSharedPtr<FJsonObject>
            {
                IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
                if (!CVar)
                {
                    return FMCPJson::MakeError(FString::Printf(TEXT("CVar '%s' not found"), *Name));
                }

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("name"), Name);
                Out->SetStringField(TEXT("string_value"), CVar->GetString());
                Out->SetNumberField(TEXT("float_value"), CVar->GetFloat());
                Out->SetNumberField(TEXT("int_value"), CVar->GetInt());
                Out->SetBoolField(TEXT("bool_value"), CVar->GetBool());
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("console"), MethodName);
}

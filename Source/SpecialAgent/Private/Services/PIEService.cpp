// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PIEService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"

FString FPIEService::GetServiceDescription() const
{
    return TEXT("Play-In-Editor control (start/stop/pause/step)");
}

TArray<FMCPToolInfo> FPIEService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("start"),
        TEXT("Request a Play-In-Editor session for the current editor world (GEditor->RequestPlaySession). Returns {success, simulate}. "
             "Params: simulate (bool, optional, default false; true starts Simulate-In-Editor — free camera, no possessed pawn — instead of Play). "
             "Workflow: async on the game thread — the PIE world is NOT usable on return; poll pie/is_playing until playing=true before issuing world/actor queries. "
             "Warning: PIE/SIE run on a DUPLICATED copy of the level (the editor world stays untouched); returns error 'PIE session already running' if one is active — call pie/stop first."))
        .OptionalBool(TEXT("simulate"), TEXT("If true, start Simulate-In-Editor (no pawn, free camera) instead of Play (default false)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("stop"),
        TEXT("Request end of the active Play-In-Editor session (GEditor->RequestEndPlayMap). Returns {success, was_playing} where was_playing reports whether a PIE world existed when called. "
             "Params: (none). "
             "Workflow: async — the PIE world tears down on a later tick; poll pie/is_playing until playing=false. "
             "Warning: any unsaved Simulate changes are discarded (PIE runs on a throwaway duplicate); harmless no-op (was_playing=false) when nothing is running."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("pause"),
        TEXT("Pause the active PIE world's gameplay clock (UGameplayStatics::SetGamePaused true). Returns {success, paused:true}. "
             "Params: (none). "
             "Workflow: pause first, then use pie/step_frame for frame-by-frame inspection or pie/resume to continue. "
             "Warning: returns error 'No active PIE session' if PIE is not running; pause freezes the gameplay tick, not the editor."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("resume"),
        TEXT("Resume the paused PIE world's gameplay clock (UGameplayStatics::SetGamePaused false). Returns {success, paused:false}. "
             "Params: (none). "
             "Workflow: use after pie/pause to let the simulation run again. "
             "Warning: returns error 'No active PIE session' if PIE is not running; safe to call even when not paused."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("step_frame"),
        TEXT("Single-step the paused PIE world by one engine frame (GEditor->PlaySessionSingleStepped), advancing on the next engine tick then re-pausing. Returns {success, delta_seconds, note}. "
             "Params: delta_seconds (number, seconds, optional, default 0.01666 = 1/60). "
             "Workflow: pie/pause first, then call repeatedly for frame-by-frame inspection. "
             "Warning: delta_seconds is ADVISORY only — the step uses the engine's real frame time, not this value; returns error 'PIE must be paused before stepping' if the world is not paused, and 'No active PIE session' if PIE is not running."))
        .OptionalNumber(TEXT("delta_seconds"), TEXT("Advisory simulated seconds to advance; the actual step uses the engine's real frame time (default 0.01666 = 1/60)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("toggle_simulate"),
        TEXT("Toggle the active PIE session between Play and Simulate modes (GEditor->RequestToggleBetweenPIEandSIE). Returns {success, was_simulating} reporting the mode BEFORE the toggle. "
             "Params: (none). "
             "Workflow: use mid-session to swap between possessing the pawn (Play) and free-camera editor selection (Simulate). "
             "Warning: returns error 'No active PIE session' if PIE is not running; both modes report world type EWorldType::PIE so use was_simulating / pie/is_playing.simulating to tell them apart."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("is_playing"),
        TEXT("Query current PIE state. Returns {success, playing (bool), paused (bool), simulating (bool)} — paused/simulating are meaningful only while playing=true. "
             "Params: (none). "
             "Workflow: poll after pie/start (until playing=true) or pie/stop (until playing=false), since both are async. "
             "Warning: read-only; editor-world actors do not tick, so live velocity/AI/movement queries are only valid while playing=true."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_world_context"),
        TEXT("Return the current world object paths. Returns {success, editor_world (string, the persistent editor world path), pie_world (string, present ONLY while PIE is running — the duplicated play world)}. "
             "Params: (none). "
             "Workflow: use to disambiguate which world subsequent calls target; absence of pie_world means no PIE session. "
             "Warning: read-only; the pie_world path changes each PIE launch (it is a fresh duplicate) so do not cache it across pie/start cycles."))
        .Build());

    return Tools;
}

FMCPResponse FPIEService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("start"))
    {
        bool bSimulate = false;
        if (Request.Params.IsValid())
        {
            FMCPJson::ReadBool(Request.Params, TEXT("simulate"), bSimulate);
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [bSimulate]() -> TSharedPtr<FJsonObject>
            {
                if (!GEditor)
                {
                    return FMCPJson::MakeError(TEXT("GEditor unavailable"));
                }
                if (GEditor->PlayWorld != nullptr)
                {
                    return FMCPJson::MakeError(TEXT("PIE session already running"));
                }

                FRequestPlaySessionParams Params;
                if (bSimulate)
                {
                    Params.WorldType = EPlaySessionWorldType::SimulateInEditor;
                }
                GEditor->RequestPlaySession(Params);

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetBoolField(TEXT("simulate"), bSimulate);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE start requested (simulate=%d)"), bSimulate ? 1 : 0);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("stop"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                if (!GEditor)
                {
                    return FMCPJson::MakeError(TEXT("GEditor unavailable"));
                }
                const bool bWasPlaying = GEditor->PlayWorld != nullptr;
                GEditor->RequestEndPlayMap();

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetBoolField(TEXT("was_playing"), bWasPlaying);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE stop requested (was_playing=%d)"), bWasPlaying ? 1 : 0);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("pause"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                if (!GEditor || !GEditor->PlayWorld)
                {
                    return FMCPJson::MakeError(TEXT("No active PIE session"));
                }
                UGameplayStatics::SetGamePaused(GEditor->PlayWorld, true);
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetBoolField(TEXT("paused"), true);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE paused"));
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("resume"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                if (!GEditor || !GEditor->PlayWorld)
                {
                    return FMCPJson::MakeError(TEXT("No active PIE session"));
                }
                UGameplayStatics::SetGamePaused(GEditor->PlayWorld, false);
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetBoolField(TEXT("paused"), false);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE resumed"));
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("step_frame"))
    {
        double DeltaSeconds = 1.0 / 60.0;
        if (Request.Params.IsValid())
        {
            FMCPJson::ReadNumber(Request.Params, TEXT("delta_seconds"), DeltaSeconds);
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [DeltaSeconds]() -> TSharedPtr<FJsonObject>
            {
                if (!GEditor || !GEditor->PlayWorld)
                {
                    return FMCPJson::MakeError(TEXT("No active PIE session"));
                }
                UWorld* World = GEditor->PlayWorld;
                if (!World->IsPaused())
                {
                    return FMCPJson::MakeError(TEXT("PIE must be paused before stepping"));
                }

                // Advance exactly one frame via the editor's supported single-step
                // path. Calling World->Tick(LEVELTICK_All, ...) directly here would
                // re-enter UWorld::Tick from inside the editor tick (this task runs
                // during FMCPGameThreadProcessor::Tick) and assert/crash, because
                // world ticking is not re-entrant. SingleStepPIE instead defers the
                // step to the engine's own tick loop and re-pauses afterwards.
                GEditor->PlaySessionSingleStepped();

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetNumberField(TEXT("delta_seconds"), DeltaSeconds);
                Out->SetStringField(TEXT("note"),
                    TEXT("Single-step requested; the frame advances on the next engine tick using the engine's real frame time (delta_seconds is advisory)."));
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE single-step requested"));
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("toggle_simulate"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                if (!GEditor || !GEditor->PlayWorld)
                {
                    return FMCPJson::MakeError(TEXT("No active PIE session"));
                }
                const bool bWasSimulating = GEditor->bIsSimulatingInEditor;
                if (bWasSimulating)
                {
                    GEditor->RequestToggleBetweenPIEandSIE();
                }
                else
                {
                    GEditor->RequestToggleBetweenPIEandSIE();
                }
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetBoolField(TEXT("was_simulating"), bWasSimulating);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE toggle_simulate (was_simulating=%d)"), bWasSimulating ? 1 : 0);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("is_playing"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                const bool bPlaying = GEditor && GEditor->PlayWorld != nullptr;
                const bool bPaused = bPlaying && GEditor->PlayWorld->IsPaused();
                const bool bSimulating = GEditor && GEditor->bIsSimulatingInEditor;
                Out->SetBoolField(TEXT("playing"), bPlaying);
                Out->SetBoolField(TEXT("paused"), bPaused);
                Out->SetBoolField(TEXT("simulating"), bSimulating);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_world_context"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                if (GEditor)
                {
                    if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
                    {
                        Out->SetStringField(TEXT("editor_world"), EditorWorld->GetPathName());
                    }
                    if (UWorld* PIEWorld = GEditor->PlayWorld)
                    {
                        Out->SetStringField(TEXT("pie_world"), PIEWorld->GetPathName());
                    }
                }
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("pie"), MethodName);
}

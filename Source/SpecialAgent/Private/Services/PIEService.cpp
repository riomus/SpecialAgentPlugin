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
        TEXT("Start Play-In-Editor session. Launches the current editor world in PIE. "
             "Params: simulate (bool, optional, start as Simulate-In-Editor instead of Play). "
             "Workflow: use pie/is_playing after to confirm; pie/stop to end. "
             "Warning: ignored if a PIE session is already active."))
        .OptionalBool(TEXT("simulate"), TEXT("If true, start Simulate-In-Editor instead of Play."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("stop"),
        TEXT("Stop the active Play-In-Editor session. Calls GEditor->RequestEndPlayMap(). "
             "Params: (none). "
             "Workflow: use pie/is_playing after to confirm. "
             "Warning: no-op when no PIE session is running."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("pause"),
        TEXT("Pause the active PIE world. Uses UGameplayStatics::SetGamePaused(true). "
             "Params: (none). "
             "Workflow: pair with pie/resume or pie/step_frame. "
             "Warning: fails if PIE is not running."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("resume"),
        TEXT("Resume the paused PIE world. Uses UGameplayStatics::SetGamePaused(false). "
             "Params: (none). "
             "Workflow: use after pie/pause. "
             "Warning: fails if PIE is not running."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("step_frame"),
        TEXT("Advance the paused PIE world by one frame. Briefly unpauses and re-pauses. "
             "Params: delta_seconds (number, optional, seconds of simulated time; default 1/60). "
             "Workflow: use after pie/pause for frame-by-frame inspection. "
             "Warning: requires PIE to be paused; fails otherwise."))
        .OptionalNumber(TEXT("delta_seconds"), TEXT("Simulated seconds to advance (default 0.0166)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("toggle_simulate"),
        TEXT("Toggle between Play and Simulate modes in the active PIE session. "
             "Params: (none). "
             "Workflow: use mid-session to swap inputs vs. editor selection. "
             "Warning: requires an active PIE session."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("is_playing"),
        TEXT("Query whether a PIE session is currently active. Returns playing (bool) and "
             "paused (bool) flags. "
             "Params: (none). "
             "Workflow: poll after pie/start or pie/stop."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_world_context"),
        TEXT("Return world context paths. editor_world (string, always present) and pie_world "
             "(string, present only while PIE is running). "
             "Params: (none). "
             "Workflow: use to disambiguate which world subsequent calls target."))
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

                // Advance one frame: unpause, tick world, re-pause.
                UGameplayStatics::SetGamePaused(World, false);
                World->Tick(LEVELTICK_All, static_cast<float>(DeltaSeconds));
                UGameplayStatics::SetGamePaused(World, true);

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetNumberField(TEXT("delta_seconds"), DeltaSeconds);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: PIE stepped %.4fs"), DeltaSeconds);
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

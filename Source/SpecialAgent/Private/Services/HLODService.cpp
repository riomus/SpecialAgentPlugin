#include "Services/HLODService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/LODActor.h"
#include "GameFramework/WorldSettings.h"
#include "HLOD/HLODSetup.h"
#include "EditorBuildUtils.h"
#include "EngineUtils.h"

FString FHLODService::GetServiceDescription() const
{
    return TEXT("Hierarchical LOD build, clear, and setup configuration");
}

FMCPResponse FHLODService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("build"))       return HandleBuild(Request);
    if (MethodName == TEXT("clear"))       return HandleClear(Request);
    if (MethodName == TEXT("set_setting")) return HandleSetSetting(Request);

    return MethodNotFound(Request.Id, TEXT("hlod"), MethodName);
}

FMCPResponse FHLODService::HandleBuild(const FMCPRequest& Request)
{
    auto Task = []() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: hlod/build no editor world"));
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        const bool bSuccess = FEditorBuildUtils::EditorBuild(
            World, FBuildOptions::BuildHierarchicalLOD, /*bAllowLightingDialog=*/false);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("success"), bSuccess);
        if (!bSuccess)
        {
            Out->SetStringField(TEXT("error"), TEXT("EditorBuild(BuildHierarchicalLOD) returned false"));
        }
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: hlod/build completed success=%d"), bSuccess);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FHLODService::HandleClear(const FMCPRequest& Request)
{
    auto Task = []() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        TArray<ALODActor*> ToDestroy;
        for (TActorIterator<ALODActor> It(World); It; ++It)
        {
            ToDestroy.Add(*It);
        }

        int32 Destroyed = 0;
        for (ALODActor* Actor : ToDestroy)
        {
            if (!Actor) continue;
            if (World->DestroyActor(Actor))
            {
                ++Destroyed;
            }
        }

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetNumberField(TEXT("destroyed"), Destroyed);
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: hlod/clear destroyed %d ALODActors"), Destroyed);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FHLODService::HandleSetSetting(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    int32 Level = 0;
    FMCPJson::ReadInteger(Request.Params, TEXT("level"), Level);
    // Bound the HLOD level: a negative index reads Setup[-1] out of bounds and
    // a huge value would grow the setup array unboundedly. UE supports a small
    // number of HLOD levels in practice.
    if (Level < 0 || Level > 7)
    {
        return InvalidParams(Request.Id, TEXT("'level' must be between 0 and 7"));
    }

    double TransitionScreenSize = -1.0;
    const bool bHasTransition = FMCPJson::ReadNumber(Request.Params, TEXT("transition_screen_size"), TransitionScreenSize);

    int32 MinActors = -1;
    const bool bHasMinActors = FMCPJson::ReadInteger(Request.Params, TEXT("min_actors"), MinActors);

    double DesiredBoundRadius = -1.0;
    const bool bHasRadius = FMCPJson::ReadNumber(Request.Params, TEXT("desired_bound_radius"), DesiredBoundRadius);

    auto Task = [Level, bHasTransition, TransitionScreenSize, bHasMinActors, MinActors, bHasRadius, DesiredBoundRadius]()
        -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AWorldSettings* Settings = World->GetWorldSettings();
        if (!Settings) return FMCPJson::MakeError(TEXT("WorldSettings unavailable"));

        TArray<FHierarchicalSimplification>& Setup = Settings->GetHierarchicalLODSetup();
        while (Setup.Num() <= Level)
        {
            Setup.Add(FHierarchicalSimplification());
        }

        FHierarchicalSimplification& Entry = Setup[Level];
        if (bHasTransition)
        {
            Entry.TransitionScreenSize = static_cast<float>(TransitionScreenSize);
        }
        if (bHasMinActors)
        {
            Entry.MinNumberOfActorsToBuild = MinActors;
        }
        if (bHasRadius)
        {
            Entry.DesiredBoundRadius = static_cast<float>(DesiredBoundRadius);
        }

        Settings->Modify();

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetNumberField(TEXT("level"), Level);
        Out->SetNumberField(TEXT("transition_screen_size"), Entry.TransitionScreenSize);
        Out->SetNumberField(TEXT("min_actors"), Entry.MinNumberOfActorsToBuild);
        Out->SetNumberField(TEXT("desired_bound_radius"), Entry.DesiredBoundRadius);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: hlod/set_setting level=%d"), Level);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FHLODService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("build"),
            TEXT("Build Hierarchical LOD (clusters source actors into ALODActor proxy meshes) for the current editor world. "
                 "Returns {success (bool), error (string, only when success is false)}. "
                 "Params: (none). "
                 "Workflow: tune clustering with hlod/set_setting first, then build; run hlod/clear before a rebuild if you want a clean slate. "
                 "Warning: long, blocking game-thread operation that may generate many proxy-mesh asset packages and triggers shader compiles; the world stays dirty until you save the level."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("clear"),
            TEXT("Destroy every ALODActor (built HLOD proxy cluster) in the current editor world. "
                 "Returns {success (bool), destroyed (integer count of ALODActors removed)}. "
                 "Params: (none). "
                 "Workflow: run before hlod/build to force a clean rebuild; safe no-op (destroyed=0) when no HLODs exist. "
                 "Warning: removes the proxy actors from the level but leaves the generated proxy-mesh assets in the Content Browser; the level stays dirty until you save it."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_setting"),
            TEXT("Configure one HierarchicalLODSetup level on the world's AWorldSettings; missing levels up to the requested index are appended. "
                 "Returns {success, level, transition_screen_size, min_actors, desired_bound_radius} reflecting the entry's current values (unspecified fields keep their existing values). "
                 "Params: level (integer, REQUIRED, 0..7 inclusive; out-of-range is rejected), "
                 "transition_screen_size (number, optional, 0..1 fraction of screen at which the cluster LOD takes over), "
                 "min_actors (integer, optional, minimum actors needed to form a cluster), "
                 "desired_bound_radius (number, optional, target cluster bounding radius in cm). "
                 "Workflow: set per level (0 = first proxy tier) before hlod/build; omit a field to leave it untouched. "
                 "Warning: only marks WorldSettings dirty (Modify), does not save and does not rebuild existing HLODs; save the level to persist and re-run hlod/build to apply."))
        .RequiredInteger(TEXT("level"),                   TEXT("HLOD level index, 0..7 inclusive (0 = first proxy tier). Out-of-range values are rejected. Missing intermediate levels are created."))
        .OptionalNumber (TEXT("transition_screen_size"),  TEXT("Screen-size fraction (0..1) at which the cluster's proxy LOD takes over"))
        .OptionalInteger(TEXT("min_actors"),              TEXT("Minimum number of source actors required to form an LODActor cluster"))
        .OptionalNumber (TEXT("desired_bound_radius"),    TEXT("Desired cluster bounding radius in cm"))
        .Build());

    return Tools;
}

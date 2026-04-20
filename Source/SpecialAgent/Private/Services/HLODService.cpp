#include "Services/HLODService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
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

FMCPResponse FHLODService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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
            TEXT("Build Hierarchical LOD for the current editor world. Invokes FEditorBuildUtils::EditorBuild with BuildHierarchicalLOD. "
                 "Params: none. "
                 "Workflow: configure hlod/set_setting first, then build. "
                 "Warning: long-running operation; blocks the editor until complete and may create many LOD asset packages."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("clear"),
            TEXT("Destroy all ALODActor instances in the current editor world. "
                 "Params: none. "
                 "Workflow: use before re-running hlod/build to force a clean rebuild. "
                 "Warning: does not remove the generated HLOD proxy assets from the content browser; call hlod/build again or delete by hand."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_setting"),
            TEXT("Set a HierarchicalLODSetup entry on WorldSettings for a given LOD level. Creates missing entries up to the requested level. "
                 "Params: level (integer, >=0 HLOD level index), transition_screen_size (number, 0..1 screen ratio), min_actors (integer, minimum cluster size), desired_bound_radius (number, cm). "
                 "Workflow: call before hlod/build to configure clustering. "
                 "Warning: modifies AWorldSettings without saving; save the level to persist."))
        .RequiredInteger(TEXT("level"),                   TEXT("HLOD level index (>=0). New slots are appended if missing."))
        .OptionalNumber (TEXT("transition_screen_size"),  TEXT("Screen radius at which the LOD actor takes over (0..1)"))
        .OptionalInteger(TEXT("min_actors"),              TEXT("Minimum number of actors required to build an LODActor cluster"))
        .OptionalNumber (TEXT("desired_bound_radius"),    TEXT("Desired cluster bounding radius in cm"))
        .Build());

    return Tools;
}

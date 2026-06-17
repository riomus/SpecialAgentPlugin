// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/EditorModeService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "EditorModeManager.h"

FString FEditorModeService::GetServiceDescription() const
{
    return TEXT("Activate editor modes (landscape/foliage/modeling) and configure brushes");
}

namespace
{
    // Resolve the supplied enum name to the corresponding FEditorModeID.
    // Returns NAME_None for unknown names.
    //
    // Uses FName literals matching the built-in mode IDs rather than the
    // FBuiltinEditorModes:: extern symbols so this TU does not require a
    // direct link against EditorFramework (UnrealEd depends on it already,
    // but the extern symbols are not re-exported by every platform linker).
    FEditorModeID ResolveModeID(const FString& Name)
    {
        if (Name == TEXT("default"))    return FEditorModeID(TEXT("EM_Default"));
        if (Name == TEXT("placement"))  return FEditorModeID(TEXT("EM_Placement"));
        if (Name == TEXT("landscape"))  return FEditorModeID(TEXT("EM_Landscape"));
        if (Name == TEXT("foliage"))    return FEditorModeID(TEXT("EM_Foliage"));
        if (Name == TEXT("mesh_paint")) return FEditorModeID(TEXT("EM_MeshPaint"));
        if (Name == TEXT("modeling"))   return FEditorModeID(TEXT("EM_ModelingToolsEditorMode"));
        return NAME_None;
    }
}

TArray<FMCPToolInfo> FEditorModeService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("activate"),
        TEXT("Switch the level editor into one tool mode, deactivating all other modes first. "
             "Returns {success, mode, mode_id, active} where active is the post-switch IsModeActive check. "
             "Params: mode (enum, required: default, placement, landscape, foliage, mesh_paint, modeling). "
             "Workflow: activate landscape/foliage before editor_mode/configure_brush; call editor_mode/get_current afterwards to verify. "
             "Warning: changes the editor UI mode (side-effecting); 'modeling' silently fails to become active if the Modeling Tools Editor Mode plugin is disabled."))
        .RequiredEnum(TEXT("mode"),
            {TEXT("default"), TEXT("placement"), TEXT("landscape"), TEXT("foliage"),
             TEXT("mesh_paint"), TEXT("modeling")},
            TEXT("Editor mode to activate (required)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_current"),
        TEXT("Report which of the well-known editor modes are currently active. "
             "Returns {success, active_modes:[{name, mode_id}]} where name is one of default/placement/landscape/foliage/mesh_paint/modeling. "
             "Params: (none). "
             "Workflow: call after editor_mode/activate to confirm the switch. "
             "Warning: read-only; only probes the six built-in modes above, so a custom/third-party mode will not appear even when active."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("configure_brush"),
        TEXT("Set the sculpt/paint brush radius and/or strength by issuing the Landscape.BrushRadius and Landscape.BrushStrength console commands. "
             "Returns {success, applied, landscape_active, foliage_active, radius?, strength?}. "
             "Params: radius (number, optional, brush radius in cm / Unreal units); strength (number, optional, 0..1). At least one of the two is required. "
             "Workflow: activate the landscape or foliage mode via editor_mode/activate first, then call this. "
             "Warning: side-effecting; only takes effect when landscape or foliage mode is active (applied=false otherwise), and both values route through the Landscape.* CVars even in foliage mode."))
        .OptionalNumber(TEXT("radius"), TEXT("Brush radius in cm / Unreal units."))
        .OptionalNumber(TEXT("strength"), TEXT("Brush strength (0..1)."))
        .Build());

    return Tools;
}

FMCPResponse FEditorModeService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("activate"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString ModeName;
        if (!FMCPJson::ReadString(Request.Params, TEXT("mode"), ModeName) || ModeName.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'mode'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [ModeName]() -> TSharedPtr<FJsonObject>
            {
                const FEditorModeID ModeID = ResolveModeID(ModeName);
                if (ModeID == NAME_None)
                {
                    return FMCPJson::MakeError(FString::Printf(TEXT("Unknown mode '%s'"), *ModeName));
                }

                FEditorModeTools& Tools = GLevelEditorModeTools();
                // Deactivate all non-default modes first so we cleanly switch.
                Tools.DeactivateAllModes();
                Tools.ActivateMode(ModeID);

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("mode"), ModeName);
                Out->SetStringField(TEXT("mode_id"), ModeID.ToString());
                Out->SetBoolField(TEXT("active"), Tools.IsModeActive(ModeID));
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Activated editor mode %s (%s)"), *ModeName, *ModeID.ToString());
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_current"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                FEditorModeTools& Tools = GLevelEditorModeTools();
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();

                // Probe the well-known modes; FEditorModeTools does not expose an
                // enumeration of active modes publicly. FName literals avoid a
                // direct dependency on EditorFramework's exported symbols.
                const TPair<const TCHAR*, FEditorModeID> Candidates[] = {
                    { TEXT("default"),    FEditorModeID(TEXT("EM_Default")) },
                    { TEXT("placement"),  FEditorModeID(TEXT("EM_Placement")) },
                    { TEXT("landscape"),  FEditorModeID(TEXT("EM_Landscape")) },
                    { TEXT("foliage"),    FEditorModeID(TEXT("EM_Foliage")) },
                    { TEXT("mesh_paint"), FEditorModeID(TEXT("EM_MeshPaint")) },
                    { TEXT("modeling"),   FEditorModeID(TEXT("EM_ModelingToolsEditorMode")) },
                };

                TArray<TSharedPtr<FJsonValue>> ActiveArr;
                for (const TPair<const TCHAR*, FEditorModeID>& Cand : Candidates)
                {
                    if (Tools.IsModeActive(Cand.Value))
                    {
                        TSharedPtr<FJsonObject> ModeObj = MakeShared<FJsonObject>();
                        ModeObj->SetStringField(TEXT("name"), Cand.Key);
                        ModeObj->SetStringField(TEXT("mode_id"), Cand.Value.ToString());
                        ActiveArr.Add(MakeShared<FJsonValueObject>(ModeObj));
                    }
                }
                Out->SetArrayField(TEXT("active_modes"), ActiveArr);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("configure_brush"))
    {
        double Radius = -1.0;
        double Strength = -1.0;
        bool bHasRadius = false;
        bool bHasStrength = false;
        if (Request.Params.IsValid())
        {
            bHasRadius   = FMCPJson::ReadNumber(Request.Params, TEXT("radius"), Radius);
            bHasStrength = FMCPJson::ReadNumber(Request.Params, TEXT("strength"), Strength);
        }
        if (!bHasRadius && !bHasStrength)
        {
            return InvalidParams(Request.Id, TEXT("Specify at least one of 'radius' or 'strength'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [Radius, Strength, bHasRadius, bHasStrength]() -> TSharedPtr<FJsonObject>
            {
                FEditorModeTools& Tools = GLevelEditorModeTools();
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();

                const FEditorModeID LandscapeID(TEXT("EM_Landscape"));
                const FEditorModeID FoliageID(TEXT("EM_Foliage"));
                const bool bLandscape = Tools.IsModeActive(LandscapeID);
                const bool bFoliage   = Tools.IsModeActive(FoliageID);

                bool bApplied = false;
                if (bLandscape || bFoliage)
                {
                    if (bHasRadius)
                    {
                        // Apply via console so we don't hard-link to LandscapeEditor module.
                        const FString Cmd = FString::Printf(TEXT("Landscape.BrushRadius %f"), Radius);
                        if (GEngine) GEngine->Exec(nullptr, *Cmd);
                        bApplied = true;
                    }
                    if (bHasStrength)
                    {
                        const FString Cmd = FString::Printf(TEXT("Landscape.BrushStrength %f"), Strength);
                        if (GEngine) GEngine->Exec(nullptr, *Cmd);
                        bApplied = true;
                    }
                }

                Out->SetBoolField(TEXT("applied"), bApplied);
                Out->SetBoolField(TEXT("landscape_active"), bLandscape);
                Out->SetBoolField(TEXT("foliage_active"), bFoliage);
                if (bHasRadius)   Out->SetNumberField(TEXT("radius"),   Radius);
                if (bHasStrength) Out->SetNumberField(TEXT("strength"), Strength);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: configure_brush applied=%d (ls=%d fol=%d)"),
                    bApplied ? 1 : 0, bLandscape ? 1 : 0, bFoliage ? 1 : 0);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("editor_mode"), MethodName);
}

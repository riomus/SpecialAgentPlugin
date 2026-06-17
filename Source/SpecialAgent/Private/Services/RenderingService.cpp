// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/RenderingService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPViewport.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
#include "Scalability.h"
#include "UnrealClient.h"

FString FRenderingService::GetServiceDescription() const
{
    return TEXT("Scalability, view modes, Nanite / Lumen toggles, high-res screenshot");
}

FMCPResponse FRenderingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("set_scalability"))      return HandleSetScalability(Request);
    if (MethodName == TEXT("set_view_mode"))        return HandleSetViewMode(Request);
    if (MethodName == TEXT("high_res_screenshot")) return HandleHighResScreenshot(Request);
    if (MethodName == TEXT("toggle_nanite"))        return HandleToggleNanite(Request);
    if (MethodName == TEXT("toggle_lumen"))         return HandleToggleLumen(Request);

    return MethodNotFound(Request.Id, TEXT("rendering"), MethodName);
}

TArray<FMCPToolInfo> FRenderingService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("set_scalability"),
        TEXT("Set engine scalability quality levels for the editor. Returns {success, view_distance, anti_aliasing, shadow, global_illumination, reflection, post_process, texture, effects, foliage, shading} -- the resulting 0-4 level of every bucket. "
             "Params: level (integer 0-4, optional; 0=low 1=medium 2=high 3=epic 4=cinematic; sets every bucket at once). Optional per-bucket overrides applied on top of level: "
             "view_distance, anti_aliasing, shadow, global_illumination, reflection, post_process, texture, effects, foliage, shading (each integer 0-4). "
             "Workflow: pass level for a quick global preset, or individual buckets for fine control; takes effect immediately in the viewport. "
             "Warning: changes the whole editor's render quality (not just PIE) and persists -- the new state is saved to GEditorSettingsIni. All values are clamped to 0-4."))
        .OptionalInteger(TEXT("level"), TEXT("Overall level 0..4 (low..cinematic)"))
        .OptionalInteger(TEXT("view_distance"),         TEXT("View distance quality 0..4"))
        .OptionalInteger(TEXT("anti_aliasing"),         TEXT("Anti-aliasing quality 0..4"))
        .OptionalInteger(TEXT("shadow"),                TEXT("Shadow quality 0..4"))
        .OptionalInteger(TEXT("global_illumination"),   TEXT("Global illumination quality 0..4"))
        .OptionalInteger(TEXT("reflection"),            TEXT("Reflection quality 0..4"))
        .OptionalInteger(TEXT("post_process"),          TEXT("Post-process quality 0..4"))
        .OptionalInteger(TEXT("texture"),               TEXT("Texture quality 0..4"))
        .OptionalInteger(TEXT("effects"),               TEXT("Effects quality 0..4"))
        .OptionalInteger(TEXT("foliage"),               TEXT("Foliage quality 0..4"))
        .OptionalInteger(TEXT("shading"),               TEXT("Shading quality 0..4"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_view_mode"),
        TEXT("Set the active Level Editor viewport's render/visualization mode. Returns {success, mode, mode_index}. "
             "Params: mode (enum, required) -- one of lit, unlit, wireframe, detail_lighting, lighting_only, "
             "shader_complexity, lightmap_density, reflections, buffer_visualization, lod_coloration, path_tracing. "
             "Workflow: set a diagnostic mode, then screenshot/capture (auto-redraws); switch back to lit when done. "
             "Warning: mutates the user's active viewport rendering. path_tracing requires hardware ray tracing enabled in Project Settings (unavailable on macOS/Metal). Sibling viewport/set_view_mode covers a different mode set (brush_wireframe, light_complexity, stationary_light_overlap)."))
        .RequiredEnum(TEXT("mode"),
            {TEXT("lit"), TEXT("unlit"), TEXT("wireframe"), TEXT("detail_lighting"),
             TEXT("lighting_only"), TEXT("shader_complexity"), TEXT("lightmap_density"),
             TEXT("reflections"), TEXT("buffer_visualization"), TEXT("lod_coloration"),
             TEXT("path_tracing")},
            TEXT("View mode name"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("high_res_screenshot"),
        TEXT("Request a high-resolution screenshot of the active viewport at a custom resolution, written to a PNG under <Project>/Saved/Screenshots/. Returns {success, width, height, show_ui, note}. "
             "Params: width (integer px, required, > 0); height (integer px, required, > 0); show_ui (bool, optional, default false; include the editor UI overlay). "
             "Workflow: for inline vision prefer screenshot/capture (returns base64 synchronously); use this only when you need a large PNG on disk. "
             "Warning: ASYNCHRONOUS -- it returns immediately and the file does NOT yet exist on disk (the engine writes it on a later frame); the success in the result only means the request was queued, not that the image is ready."))
        .RequiredInteger(TEXT("width"),  TEXT("Screenshot width in pixels (> 0)"))
        .RequiredInteger(TEXT("height"), TEXT("Screenshot height in pixels (> 0)"))
        .OptionalBool(TEXT("show_ui"),   TEXT("Include the editor UI overlay in the screenshot (default false)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("toggle_nanite"),
        TEXT("Enable or disable Nanite rendering globally via the r.Nanite console variable. Returns {success, enabled}. "
             "Params: enabled (bool, required; true sets r.Nanite=1, false sets r.Nanite=0). "
             "Workflow: disable, then screenshot/capture to compare detail/performance/shadow behavior, then re-enable. "
             "Warning: this is a global engine CVar affecting the whole editor (not per-actor or PIE-only); it takes effect on the next frame and returns an error if r.Nanite is not registered."))
        .RequiredBool(TEXT("enabled"), TEXT("true to enable Nanite (r.Nanite=1), false to disable (r.Nanite=0)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("toggle_lumen"),
        TEXT("Enable or disable Lumen dynamic global illumination and reflections via r.DynamicGlobalIlluminationMethod and r.ReflectionMethod. Returns {success, enabled, gi_method, reflection_method}. "
             "Params: enabled (bool, required). When true: GI method = 1 (Lumen), reflection method = 1 (Lumen). When false: GI method = 0 (None) and reflection method = 2 (Screen Space) as the fallback. "
             "Workflow: disable, then screenshot/capture to compare against the Lumen-lit look, then re-enable (Lumen is the UE5.7 default GI/reflection method). "
             "Warning: global engine CVars affecting the whole editor (not per-actor); disabling does not produce baked lighting -- it just drops dynamic GI and falls back to screen-space reflections. Returns an error if the CVars are not found."))
        .RequiredBool(TEXT("enabled"), TEXT("true to enable Lumen GI + reflections, false to disable (GI None, reflections Screen Space)"))
        .Build());

    return Tools;
}

namespace
{
    bool ApplyLevelIfPresent(const TSharedPtr<FJsonObject>& Params, const FString& Field, TFunctionRef<void(int32)> Setter)
    {
        int32 Value = 0;
        if (FMCPJson::ReadInteger(Params, Field, Value))
        {
            Setter(FMath::Clamp(Value, 0, 4));
            return true;
        }
        return false;
    }
}

FMCPResponse FRenderingService::HandleSetScalability(const FMCPRequest& Request)
{
    TSharedPtr<FJsonObject> Params = Request.Params.IsValid() ? Request.Params : MakeShared<FJsonObject>();

    auto Task = [Params]() -> TSharedPtr<FJsonObject>
    {
        Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();

        int32 OverallLevel = -1;
        if (FMCPJson::ReadInteger(Params, TEXT("level"), OverallLevel))
        {
            Levels.SetFromSingleQualityLevel(FMath::Clamp(OverallLevel, 0, 4));
        }

        ApplyLevelIfPresent(Params, TEXT("view_distance"),       [&](int32 V){ Levels.SetViewDistanceQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("anti_aliasing"),       [&](int32 V){ Levels.SetAntiAliasingQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("shadow"),              [&](int32 V){ Levels.SetShadowQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("global_illumination"), [&](int32 V){ Levels.SetGlobalIlluminationQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("reflection"),          [&](int32 V){ Levels.SetReflectionQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("post_process"),        [&](int32 V){ Levels.SetPostProcessQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("texture"),             [&](int32 V){ Levels.SetTextureQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("effects"),             [&](int32 V){ Levels.SetEffectsQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("foliage"),             [&](int32 V){ Levels.SetFoliageQuality(V); });
        ApplyLevelIfPresent(Params, TEXT("shading"),             [&](int32 V){ Levels.SetShadingQuality(V); });

        Scalability::SetQualityLevels(Levels);
        Scalability::SaveState(GEditorSettingsIni);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetNumberField(TEXT("view_distance"),        Levels.ViewDistanceQuality);
        Result->SetNumberField(TEXT("anti_aliasing"),        Levels.AntiAliasingQuality);
        Result->SetNumberField(TEXT("shadow"),               Levels.ShadowQuality);
        Result->SetNumberField(TEXT("global_illumination"),  Levels.GlobalIlluminationQuality);
        Result->SetNumberField(TEXT("reflection"),           Levels.ReflectionQuality);
        Result->SetNumberField(TEXT("post_process"),         Levels.PostProcessQuality);
        Result->SetNumberField(TEXT("texture"),              Levels.TextureQuality);
        Result->SetNumberField(TEXT("effects"),              Levels.EffectsQuality);
        Result->SetNumberField(TEXT("foliage"),              Levels.FoliageQuality);
        Result->SetNumberField(TEXT("shading"),              Levels.ShadingQuality);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderingService::HandleSetViewMode(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    FString Mode;
    if (!FMCPJson::ReadString(Request.Params, TEXT("mode"), Mode))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'mode'"));
    }

    EViewModeIndex ModeIndex = VMI_Lit;
    if      (Mode == TEXT("lit"))                  ModeIndex = VMI_Lit;
    else if (Mode == TEXT("unlit"))                ModeIndex = VMI_Unlit;
    else if (Mode == TEXT("wireframe"))            ModeIndex = VMI_Wireframe;
    else if (Mode == TEXT("detail_lighting"))      ModeIndex = VMI_Lit_DetailLighting;
    else if (Mode == TEXT("lighting_only"))        ModeIndex = VMI_LightingOnly;
    else if (Mode == TEXT("shader_complexity"))    ModeIndex = VMI_ShaderComplexity;
    else if (Mode == TEXT("lightmap_density"))     ModeIndex = VMI_LightmapDensity;
    else if (Mode == TEXT("reflections"))          ModeIndex = VMI_ReflectionOverride;
    else if (Mode == TEXT("buffer_visualization")) ModeIndex = VMI_VisualizeBuffer;
    else if (Mode == TEXT("lod_coloration"))       ModeIndex = VMI_LODColoration;
    else if (Mode == TEXT("path_tracing"))         ModeIndex = VMI_PathTracing;
    else
    {
        return InvalidParams(Request.Id, FString::Printf(TEXT("Unknown view mode '%s'"), *Mode));
    }

    auto Task = [Mode, ModeIndex]() -> TSharedPtr<FJsonObject>
    {
        FLevelEditorViewportClient* Viewport = FMCPViewport::GetLevelClient();
        if (!Viewport)
        {
            return FMCPJson::MakeError(TEXT("No active level editor viewport"));
        }

        Viewport->SetViewMode(ModeIndex);
        Viewport->Invalidate();

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetStringField(TEXT("mode"), Mode);
        Result->SetNumberField(TEXT("mode_index"), static_cast<int32>(ModeIndex));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderingService::HandleHighResScreenshot(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    int32 Width = 0, Height = 0;
    if (!FMCPJson::ReadInteger(Request.Params, TEXT("width"), Width) || Width <= 0)
    {
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'width'"));
    }
    if (!FMCPJson::ReadInteger(Request.Params, TEXT("height"), Height) || Height <= 0)
    {
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'height'"));
    }

    bool bShowUI = false;
    FMCPJson::ReadBool(Request.Params, TEXT("show_ui"), bShowUI);

    auto Task = [Width, Height, bShowUI]() -> TSharedPtr<FJsonObject>
    {
        GScreenshotResolutionX = static_cast<uint32>(Width);
        GScreenshotResolutionY = static_cast<uint32>(Height);
        FScreenshotRequest::RequestScreenshot(bShowUI);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetNumberField(TEXT("width"), Width);
        Result->SetNumberField(TEXT("height"), Height);
        Result->SetBoolField(TEXT("show_ui"), bShowUI);
        Result->SetStringField(TEXT("note"), TEXT("Saved asynchronously to Saved/Screenshots/"));
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderingService::HandleToggleNanite(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    bool bEnabled = false;
    if (!FMCPJson::ReadBool(Request.Params, TEXT("enabled"), bEnabled))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'enabled'"));
    }

    auto Task = [bEnabled]() -> TSharedPtr<FJsonObject>
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite")))
        {
            CVar->Set(bEnabled ? 1 : 0, ECVF_SetByConsole);
        }
        else
        {
            return FMCPJson::MakeError(TEXT("CVar r.Nanite not found"));
        }

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetBoolField(TEXT("enabled"), bEnabled);
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FRenderingService::HandleToggleLumen(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
    {
        return InvalidParams(Request.Id, TEXT("Missing params"));
    }

    bool bEnabled = false;
    if (!FMCPJson::ReadBool(Request.Params, TEXT("enabled"), bEnabled))
    {
        return InvalidParams(Request.Id, TEXT("Missing 'enabled'"));
    }

    auto Task = [bEnabled]() -> TSharedPtr<FJsonObject>
    {
        // Dynamic GI method: 1 = Lumen, 0 = None.
        // Reflection method:  1 = Lumen, 2 = Screen Space. Toggling off uses SSR as the fallback.
        IConsoleVariable* GICVar   = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        IConsoleVariable* ReflCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ReflectionMethod"));
        if (!GICVar || !ReflCVar)
        {
            return FMCPJson::MakeError(TEXT("Lumen CVars not found (r.DynamicGlobalIlluminationMethod / r.ReflectionMethod)"));
        }

        GICVar->Set(bEnabled ? 1 : 0, ECVF_SetByConsole);
        ReflCVar->Set(bEnabled ? 1 : 2, ECVF_SetByConsole);

        TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
        Result->SetBoolField(TEXT("enabled"), bEnabled);
        Result->SetNumberField(TEXT("gi_method"),         GICVar->GetInt());
        Result->SetNumberField(TEXT("reflection_method"), ReflCVar->GetInt());
        return Result;
    };

    TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/RenderingService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

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

FMCPResponse FRenderingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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
        TEXT("Set engine scalability quality levels. Applies via Scalability::SetQualityLevels. "
             "Params: level (integer, 0=low 1=medium 2=high 3=epic 4=cinematic). Optional per-bucket overrides: "
             "view_distance, anti_aliasing, shadow, global_illumination, reflection, post_process, texture, "
             "effects, foliage, shading (each integer 0-4). "
             "Workflow: set once per editor session; visible immediately in viewport. "
             "Warning: affects the whole editor, not just PIE."))
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
        TEXT("Set the editor viewport view mode (lit, unlit, wireframe, etc.). "
             "Params: mode (string enum: lit, unlit, wireframe, detail_lighting, lighting_only, "
             "shader_complexity, lightmap_density, reflections, buffer_visualization, lod_coloration, path_tracing). "
             "Workflow: set before capturing screenshots for diagnostic views. "
             "Warning: path_tracing requires hardware ray-tracing enabled in project settings."))
        .RequiredEnum(TEXT("mode"),
            {TEXT("lit"), TEXT("unlit"), TEXT("wireframe"), TEXT("detail_lighting"),
             TEXT("lighting_only"), TEXT("shader_complexity"), TEXT("lightmap_density"),
             TEXT("reflections"), TEXT("buffer_visualization"), TEXT("lod_coloration"),
             TEXT("path_tracing")},
            TEXT("View mode name"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("high_res_screenshot"),
        TEXT("Request a high-resolution screenshot of the active viewport with custom resolution. "
             "Params: width (integer, pixels), height (integer, pixels), show_ui (bool, optional). "
             "Workflow: triggers the engine's built-in HRES path; saved to Saved/Screenshots. "
             "Warning: asynchronous — does not wait for the file to be written."))
        .RequiredInteger(TEXT("width"),  TEXT("Screenshot width in pixels"))
        .RequiredInteger(TEXT("height"), TEXT("Screenshot height in pixels"))
        .OptionalBool(TEXT("show_ui"),   TEXT("Include UI overlay in screenshot (default false)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("toggle_nanite"),
        TEXT("Enable or disable Nanite rendering via the r.Nanite CVar. "
             "Params: enabled (bool). "
             "Workflow: toggle off to compare performance / shadow behavior. "
             "Warning: global engine setting; takes effect on next frame."))
        .RequiredBool(TEXT("enabled"), TEXT("True to enable Nanite, false to disable"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("toggle_lumen"),
        TEXT("Enable or disable Lumen GI & reflections via r.DynamicGlobalIlluminationMethod and r.ReflectionMethod. "
             "Params: enabled (bool). "
             "Workflow: toggle to compare lit scene vs. baked-only. "
             "Warning: switches GI method 1 (Lumen) vs 0 (None) and Reflection method 1 vs 2."))
        .RequiredBool(TEXT("enabled"), TEXT("True to enable Lumen, false to disable"))
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
        FLevelEditorViewportClient* Viewport = GCurrentLevelEditingViewportClient;
        if (!Viewport && GEditor && GEditor->GetActiveViewport())
        {
            Viewport = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
        }
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

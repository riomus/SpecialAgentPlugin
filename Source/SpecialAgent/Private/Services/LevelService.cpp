// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/LevelService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FileHelpers.h"

FString FLevelService::GetServiceDescription() const
{
    return TEXT("Open / new / save-as level files");
}

TArray<FMCPToolInfo> FLevelService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("open"),
        TEXT("Open (load) an existing level by package path. "
             "Params: map_path (string, required, content-path e.g. '/Game/Maps/MyLevel'). "
             "Workflow: use level/get_current_path after to confirm. "
             "Warning: unsaved edits in the current level are prompted by the editor."))
        .RequiredString(TEXT("map_path"), TEXT("Level package path (e.g. /Game/Maps/MyLevel)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("new"),
        TEXT("Create a new untitled blank map in the editor. "
             "Params: save_existing (bool, optional, save current before replacing; default false). "
             "Workflow: follow with level/save_as to write to disk. "
             "Warning: discards unsaved work if save_existing is false."))
        .OptionalBool(TEXT("save_existing"), TEXT("If true, prompt/save current map before creating new."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("save_as"),
        TEXT("Save the current level as a new asset. The editor shows a path dialog. "
             "Workflow: use after level/new or to fork an existing level. "
             "Warning: returns success=false if the user cancels the dialog."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_current_path"),
        TEXT("Return the package name / file path of the currently open level. "
             "Workflow: call before level/open to compare."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_templates"),
        TEXT("Return the built-in new-level templates recognized by this service. "
             "Workflow: pick one and feed its name to level/new when template support is added."))
        .Build());

    return Tools;
}

FMCPResponse FLevelService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("open"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString MapPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("map_path"), MapPath) || MapPath.IsEmpty())
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter 'map_path'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [MapPath]() -> TSharedPtr<FJsonObject>
            {
                UWorld* Loaded = UEditorLoadingAndSavingUtils::LoadMap(MapPath);
                if (!Loaded)
                {
                    return FMCPJson::MakeError(FString::Printf(TEXT("Failed to load map '%s'"), *MapPath));
                }

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("map_path"), MapPath);
                Out->SetStringField(TEXT("world_path"), Loaded->GetPathName());
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Opened map '%s'"), *MapPath);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("new"))
    {
        bool bSaveExisting = false;
        if (Request.Params.IsValid())
        {
            FMCPJson::ReadBool(Request.Params, TEXT("save_existing"), bSaveExisting);
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [bSaveExisting]() -> TSharedPtr<FJsonObject>
            {
                UWorld* NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(bSaveExisting);
                if (!NewWorld)
                {
                    return FMCPJson::MakeError(TEXT("NewBlankMap returned null"));
                }

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("world_path"), NewWorld->GetPathName());
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Created new blank map %s"), *NewWorld->GetPathName());
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("save_as"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
                if (!World || !World->PersistentLevel)
                {
                    return FMCPJson::MakeError(TEXT("No active editor world / level"));
                }
                FString SavedFilename;
                const bool bSaved = FEditorFileUtils::SaveLevelAs(World->PersistentLevel, &SavedFilename);
                if (!bSaved)
                {
                    return FMCPJson::MakeError(TEXT("SaveLevelAs failed or was cancelled"));
                }

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("saved_filename"), SavedFilename);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Saved level as '%s'"), *SavedFilename);
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_current_path"))
    {
        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            []() -> TSharedPtr<FJsonObject>
            {
                UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
                if (!World)
                {
                    return FMCPJson::MakeError(TEXT("No active editor world"));
                }
                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("world_path"), World->GetPathName());
                if (UPackage* Pkg = World->GetPackage())
                {
                    Out->SetStringField(TEXT("package_name"), Pkg->GetName());
                }
                return Out;
            });

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("list_templates"))
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
        Add(TEXT("Empty"),        TEXT("Blank map with no preset actors."));
        Add(TEXT("Basic"),        TEXT("Floor, lights, sky, player start."));
        Add(TEXT("OpenWorld"),    TEXT("World-partition enabled open-world template."));
        Add(TEXT("VR-Basic"),     TEXT("VR-ready blank room."));
        Add(TEXT("TimeOfDay"),    TEXT("Basic with movable directional sun, atmosphere, fog."));
        Out->SetArrayField(TEXT("templates"), Arr);
        return FMCPResponse::Success(Request.Id, Out);
    }

    return MethodNotFound(Request.Id, TEXT("level"), MethodName);
}

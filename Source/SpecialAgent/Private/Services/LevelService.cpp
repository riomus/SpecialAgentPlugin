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
#include "Misc/PackageName.h"

FString FLevelService::GetServiceDescription() const
{
    return TEXT("Open / new / save-as level files");
}

TArray<FMCPToolInfo> FLevelService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("open"),
        TEXT("Load an existing level into the editor by virtual package path, replacing the current world. "
             "Returns {success, map_path, world_path}. "
             "Params: map_path (string, required, virtual content path like '/Game/Maps/MyLevel', no extension, never an OS path). "
             "Workflow: call level/get_current_path afterwards to confirm the active world. "
             "Warning: switches the active editor world; the editor prompts to save unsaved edits in the current level first."))
        .RequiredString(TEXT("map_path"), TEXT("Virtual level package path (e.g. /Game/Maps/MyLevel)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("new"),
        TEXT("Create a new untitled blank map and make it the active editor world (in memory only, not yet on disk). "
             "Returns {success, world_path}. "
             "Params: save_existing (bool, optional, default false; when true the editor prompts to save the current map before replacing it). "
             "Workflow: follow with level/save_as to write the new map to a virtual content path. "
             "Warning: discards unsaved work in the current level when save_existing is false; the new map is transient until saved."))
        .OptionalBool(TEXT("save_existing"), TEXT("If true, prompt to save the current map before creating the new one (default false)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("save_as"),
        TEXT("Save the current editor level to disk at a caller-supplied path, non-modal (no Save-As dialog) so it is safe for unattended editor automation. "
             "Returns {success, saved_path} where saved_path is the on-disk OS path written. "
             "Params: package_path (string, virtual content path like '/Game/Maps/MyLevel', no extension) OR file_path (string, absolute OS path to the .umap to write); exactly one is required. "
             "Workflow: use after level/new to persist an untitled map, or to fork an existing level; pass package_path for normal /Game/... maps. "
             "Warning: overwrites any existing file at the resolved path without prompting; returns an error result when no path is given, the path cannot be resolved, or there is no active world."))
        .OptionalString(TEXT("package_path"), TEXT("Virtual level package path to save to (e.g. /Game/Maps/MyLevel, no extension). Provide this OR file_path."))
        .OptionalString(TEXT("file_path"), TEXT("Absolute OS path of the .umap file to write. Provide this OR package_path."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_current_path"),
        TEXT("Return the world object path and virtual package name of the currently open editor level. "
             "Returns {success, world_path, package_name} (package_name is the /Game/... virtual path, no extension). "
             "Params: (none). "
             "Workflow: call before level/open to compare, or after level/open to confirm the switch took effect. "
             "Warning: read-only; returns an error result when there is no active editor world (e.g. during startup)."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_templates"),
        TEXT("Return the static catalog of new-level template names this service documents (Empty, Basic, OpenWorld, VR-Basic, TimeOfDay). "
             "Returns {success, templates:[{name, description}]}. "
             "Params: (none). "
             "Workflow: informational only; level/new currently always creates a blank map and does NOT accept a template argument. "
             "Warning: read-only; these names are not yet wired into level/new, so picking one has no effect on map creation."))
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
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object; require 'package_path' or 'file_path'"));
        }

        // Resolve the destination on-disk filename up front (off the game thread).
        // Accept either a raw OS file_path (passed straight to SaveMap) or a
        // virtual package_path (/Game/...) which we convert to a .umap filename.
        FString SaveFilename;
        FString PackagePath;
        if (FMCPJson::ReadString(Request.Params, TEXT("file_path"), SaveFilename) && !SaveFilename.IsEmpty())
        {
            // Use the explicit OS path as-is.
        }
        else if (FMCPJson::ReadString(Request.Params, TEXT("package_path"), PackagePath) && !PackagePath.IsEmpty())
        {
            if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, SaveFilename, FPackageName::GetMapPackageExtension()))
            {
                return InvalidParams(Request.Id, FString::Printf(TEXT("Could not resolve 'package_path' '%s' to a map filename"), *PackagePath));
            }
        }
        else
        {
            return InvalidParams(Request.Id, TEXT("Missing required parameter: provide 'package_path' or 'file_path'"));
        }

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(
            [SaveFilename]() -> TSharedPtr<FJsonObject>
            {
                UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
                if (!World || !World->PersistentLevel)
                {
                    return FMCPJson::MakeError(TEXT("No active editor world / level"));
                }

                // Non-modal save to the caller-supplied filename (no Save-As dialog).
                const bool bSaved = FEditorFileUtils::SaveMap(World, SaveFilename);
                if (!bSaved)
                {
                    return FMCPJson::MakeError(FString::Printf(TEXT("SaveMap failed for '%s'"), *SaveFilename));
                }

                TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
                Out->SetStringField(TEXT("saved_path"), SaveFilename);
                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Saved level to '%s'"), *SaveFilename);
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

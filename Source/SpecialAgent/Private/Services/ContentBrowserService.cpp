#include "Services/ContentBrowserService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "EditorAssetLibrary.h"
#include "IContentBrowserSingleton.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Editor.h"

namespace
{
    IContentBrowserSingleton& GetContentBrowser()
    {
        FContentBrowserModule& Module = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
        return Module.Get();
    }

    UEditorAssetSubsystem* GetAssetSubsystem()
    {
        if (GEditor)
        {
            return GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
        }
        return nullptr;
    }

    void SyncToAssetPath(const FString& AssetPath)
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
        FAssetData Data = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
        if (Data.IsValid())
        {
            TArray<FAssetData> List{Data};
            GetContentBrowser().SyncBrowserToAssets(List, /*bAllowLocked*/ false, /*bFocus*/ true);
        }
    }
}

FString FContentBrowserService::GetServiceDescription() const
{
    return TEXT("Content Browser UI operations (sync, folders, metadata) — UI-focused counterpart to assets service");
}

FMCPResponse FContentBrowserService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    if (MethodName == TEXT("sync_to_folder"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString FolderPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("folder_path"), FolderPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'folder_path'"));
        }

        auto Task = [FolderPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            TArray<FString> Folders{FolderPath};
            GetContentBrowser().SyncBrowserToFolders(Folders, /*bAllowLocked*/ false, /*bFocus*/ true);

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("folder_path"), FolderPath);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/sync_to_folder '%s'"), *FolderPath);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("create_folder"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString FolderPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("folder_path"), FolderPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'folder_path'"));
        }

        auto Task = [FolderPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            const bool bOk = UEditorAssetLibrary::MakeDirectory(FolderPath);
            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("folder_path"), FolderPath);
            if (!bOk)
            {
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("MakeDirectory failed for '%s'"), *FolderPath));
            }
            else
            {
                TArray<FString> Folders{FolderPath};
                GetContentBrowser().SyncBrowserToFolders(Folders, /*bAllowLocked*/ false, /*bFocus*/ true);
            }
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/create_folder '%s' %s"), *FolderPath, bOk ? TEXT("OK") : TEXT("FAIL"));
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("rename"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString SourcePath;
        FString DestinationPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("source_path"), SourcePath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'source_path' or 'destination_path'"));
        }

        auto Task = [SourcePath, DestinationPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            const bool bOk = UEditorAssetLibrary::RenameAsset(SourcePath, DestinationPath);
            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("source_path"), SourcePath);
            Result->SetStringField(TEXT("destination_path"), DestinationPath);
            if (!bOk)
            {
                Result->SetStringField(TEXT("error"), TEXT("RenameAsset failed"));
            }
            else
            {
                SyncToAssetPath(DestinationPath);
            }
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/rename '%s' → '%s' %s"),
                *SourcePath, *DestinationPath, bOk ? TEXT("OK") : TEXT("FAIL"));
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("delete"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString AssetPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
        }

        auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            const bool bOk = UEditorAssetLibrary::DeleteAsset(AssetPath);
            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            if (!bOk)
            {
                Result->SetStringField(TEXT("error"), TEXT("DeleteAsset failed"));
            }
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/delete '%s' %s"),
                *AssetPath, bOk ? TEXT("OK") : TEXT("FAIL"));
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("move"))
    {
        // Move is implemented as Rename to a new location.
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString SourcePath;
        FString DestinationPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("source_path"), SourcePath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'source_path' or 'destination_path'"));
        }

        auto Task = [SourcePath, DestinationPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            const bool bOk = UEditorAssetLibrary::RenameAsset(SourcePath, DestinationPath);
            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("source_path"), SourcePath);
            Result->SetStringField(TEXT("destination_path"), DestinationPath);
            if (!bOk)
            {
                Result->SetStringField(TEXT("error"), TEXT("Move (RenameAsset) failed"));
            }
            else
            {
                SyncToAssetPath(DestinationPath);
            }
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/move '%s' → '%s' %s"),
                *SourcePath, *DestinationPath, bOk ? TEXT("OK") : TEXT("FAIL"));
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("duplicate"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString SourcePath;
        FString DestinationPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("source_path"), SourcePath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'source_path' or 'destination_path'"));
        }

        auto Task = [SourcePath, DestinationPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UObject* Duplicated = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestinationPath);
            const bool bOk = Duplicated != nullptr;
            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("source_path"), SourcePath);
            Result->SetStringField(TEXT("destination_path"), DestinationPath);
            if (!bOk)
            {
                Result->SetStringField(TEXT("error"), TEXT("DuplicateAsset returned null"));
            }
            else
            {
                SyncToAssetPath(Duplicated->GetPathName());
            }
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/duplicate '%s' → '%s' %s"),
                *SourcePath, *DestinationPath, bOk ? TEXT("OK") : TEXT("FAIL"));
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("save"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString AssetPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
        }

        bool bOnlyIfDirty = true;
        FMCPJson::ReadBool(Request.Params, TEXT("only_if_dirty"), bOnlyIfDirty);

        auto Task = [AssetPath, bOnlyIfDirty]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            const bool bOk = UEditorAssetLibrary::SaveAsset(AssetPath, bOnlyIfDirty);
            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            if (!bOk)
            {
                Result->SetStringField(TEXT("error"), TEXT("SaveAsset failed"));
            }
            else
            {
                SyncToAssetPath(AssetPath);
            }
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/save '%s' %s"),
                *AssetPath, bOk ? TEXT("OK") : TEXT("FAIL"));
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("set_metadata"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString AssetPath;
        FString Tag;
        FString Value;
        if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("tag"), Tag) ||
            !FMCPJson::ReadString(Request.Params, TEXT("value"), Value))
        {
            return InvalidParams(Request.Id, TEXT("Missing one of: asset_path, tag, value"));
        }

        auto Task = [AssetPath, Tag, Value]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
            if (!Asset)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
                return Result;
            }
            UEditorAssetSubsystem* Sub = GetAssetSubsystem();
            if (!Sub)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), TEXT("EditorAssetSubsystem unavailable"));
                return Result;
            }
            Sub->SetMetadataTag(Asset, FName(*Tag), Value);
            // Mark dirty so Save picks it up.
            Asset->MarkPackageDirty();

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            Result->SetStringField(TEXT("tag"), Tag);
            Result->SetStringField(TEXT("value"), Value);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: content_browser/set_metadata '%s' %s=%s"),
                *AssetPath, *Tag, *Value);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_metadata"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString AssetPath;
        FString Tag;
        if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("tag"), Tag))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'asset_path' or 'tag'"));
        }

        auto Task = [AssetPath, Tag]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
            if (!Asset)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
                return Result;
            }
            UEditorAssetSubsystem* Sub = GetAssetSubsystem();
            if (!Sub)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), TEXT("EditorAssetSubsystem unavailable"));
                return Result;
            }
            const FString Value = Sub->GetMetadataTag(Asset, FName(*Tag));
            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            Result->SetStringField(TEXT("tag"), Tag);
            Result->SetStringField(TEXT("value"), Value);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("content_browser"), MethodName);
}

TArray<FMCPToolInfo> FContentBrowserService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("sync_to_folder"),
        TEXT("Focus the Content Browser on a folder. UI-level sync, no data mutation.\n"
             "Params: folder_path (string, /Game/... content path).\n"
             "Workflow: After create_folder, call this to reveal the new folder."))
        .RequiredString(TEXT("folder_path"), TEXT("Content-browser folder (e.g. /Game/MyStuff)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("create_folder"),
        TEXT("Create a new folder in the content browser and focus it.\n"
             "Params: folder_path (string, /Game/... path to create).\n"
             "Workflow: Follow with asset_import/import_fbx targeting this folder.\n"
             "Warning: Parent path must exist (creates one level)."))
        .RequiredString(TEXT("folder_path"), TEXT("Folder path to create (e.g. /Game/NewFolder)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("rename"),
        TEXT("Rename (or move) an asset by full object path, then focus the new location.\n"
             "Params: source_path (string, current asset path), destination_path (string, new asset path).\n"
             "Workflow: Differs from 'move' only in semantic intent; both use RenameAsset."))
        .RequiredString(TEXT("source_path"), TEXT("Existing asset path (e.g. /Game/Foo.Foo)"))
        .RequiredString(TEXT("destination_path"), TEXT("New asset path (e.g. /Game/Bar.Bar)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("delete"),
        TEXT("Force-delete an asset (no reference check). Skips the UI confirmation dialog.\n"
             "Params: asset_path (string, /Game/... asset path).\n"
             "Warning: Destructive. Closes any open asset editor for this asset."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset path to delete (e.g. /Game/Old.Old)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("move"),
        TEXT("Move an asset to a new path and focus its new location.\n"
             "Params: source_path (string), destination_path (string).\n"
             "Workflow: Equivalent to rename but intended for path changes that preserve the asset name."))
        .RequiredString(TEXT("source_path"), TEXT("Existing asset path"))
        .RequiredString(TEXT("destination_path"), TEXT("New asset path"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("duplicate"),
        TEXT("Duplicate an asset to a new path and focus the duplicate in the content browser.\n"
             "Params: source_path (string), destination_path (string)."))
        .RequiredString(TEXT("source_path"), TEXT("Source asset path"))
        .RequiredString(TEXT("destination_path"), TEXT("New asset path for the duplicate"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("save"),
        TEXT("Save the asset's package to disk and focus it in the content browser.\n"
             "Params: asset_path (string), only_if_dirty (bool, default true).\n"
             "Workflow: Call after set_metadata or any set_* that doesn't auto-save."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset path to save"))
        .OptionalBool(TEXT("only_if_dirty"), TEXT("Skip save if asset is not dirty (default: true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_metadata"),
        TEXT("Set a key/value metadata tag on a loaded asset. Tags are stored in the package.\n"
             "Params: asset_path (string), tag (string, FName-compatible), value (string).\n"
             "Workflow: Follow with content_browser/save to persist. Use get_metadata to verify.\n"
             "Warning: Tags are NOT the same as Asset Registry tags (which are read-only from code)."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset path"))
        .RequiredString(TEXT("tag"), TEXT("Metadata key (FName)"))
        .RequiredString(TEXT("value"), TEXT("Metadata value (string)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_metadata"),
        TEXT("Read a metadata tag from a loaded asset. Returns empty string if absent.\n"
             "Params: asset_path (string), tag (string, FName-compatible)."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset path"))
        .RequiredString(TEXT("tag"), TEXT("Metadata key"))
        .Build());

    return Tools;
}

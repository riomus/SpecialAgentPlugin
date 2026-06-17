#include "Services/ContentBrowserService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

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

FMCPResponse FContentBrowserService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
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
        TEXT("Navigate and focus the Content Browser on a folder. Returns {success, folder_path}. "
             "Params: folder_path (string, required, virtual content path /Game/...). "
             "Workflow: after content_browser/create_folder, call this to reveal the new folder. "
             "Warning: UI-only, mutates nothing; no-op effect in headless/cooked runs with no Content Browser."))
        .RequiredString(TEXT("folder_path"), TEXT("Virtual content folder, e.g. /Game/MyStuff"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("create_folder"),
        TEXT("Create a folder (virtual /Game path) and focus it in the Content Browser. Returns {success, folder_path}. "
             "Params: folder_path (string, required, virtual content path to create, e.g. /Game/NewFolder). "
             "Workflow: follow with asset_import or content_browser/duplicate targeting this folder. "
             "Warning: idempotent - succeeds if the folder already exists; creates intermediate parent folders as needed. The folder is empty until an asset is saved into it."))
        .RequiredString(TEXT("folder_path"), TEXT("Virtual content path to create, e.g. /Game/NewFolder"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("rename"),
        TEXT("Rename or move an asset to a new full object path, then focus it. Returns {success, source_path, destination_path}. "
             "Params: source_path (string, required, current object path /Game/Foo.Foo), destination_path (string, required, new FULL object path including the asset name /Game/Bar.Bar). "
             "Workflow: identical behavior to content_browser/move (both rename the asset); pick the name that reads best - changing the folder, the name, or both is all supported here. "
             "Warning: mutates and dirties the package (call content_browser/save afterward) and leaves a redirector at the old path until referencers are fixed up."))
        .RequiredString(TEXT("source_path"), TEXT("Existing asset object path, e.g. /Game/Foo.Foo"))
        .RequiredString(TEXT("destination_path"), TEXT("New full object path including the asset name, e.g. /Game/Bar.Bar"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("delete"),
        TEXT("Force-delete an asset by object path with NO reference check and no confirmation dialog. Returns {success, asset_path}. "
             "Params: asset_path (string, required, object path /Game/Old.Old). "
             "Workflow: run asset_deps/get_referencers first; use assets/delete instead when you want the safe reference-checked path that refuses referenced assets. "
             "Warning: destructive and irreversible; deleting a referenced asset breaks its referencers (they get null references) and closes any open editor for the asset."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset object path to delete, e.g. /Game/Old.Old"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("move"),
        TEXT("Move an asset to a new full object path and focus it. Returns {success, source_path, destination_path}. "
             "Params: source_path (string, required, existing object path), destination_path (string, required, FULL destination object path including the asset name, e.g. /Game/New/Foo.Foo). "
             "Workflow: call content_browser/create_folder first if the target folder is new; behaves identically to content_browser/rename (both call the same rename). "
             "Warning: mutates and dirties packages (save afterward); leaves a redirector at the source until referencers are fixed up."))
        .RequiredString(TEXT("source_path"), TEXT("Existing asset object path"))
        .RequiredString(TEXT("destination_path"), TEXT("Full destination object path including the asset name"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("duplicate"),
        TEXT("Duplicate an asset to a new full object path and focus the copy. Returns {success, source_path, destination_path}. "
             "Params: source_path (string, required, object path), destination_path (string, required, FULL target object path including the new asset name, e.g. /Game/New/Foo.Foo). "
             "Workflow: call content_browser/create_folder first if the target folder is new, then content_browser/save to persist the copy. "
             "Warning: the copy starts unsaved; editing a duplicated material recompiles all its shader expressions - prefer modifying the parent over duplicating."))
        .RequiredString(TEXT("source_path"), TEXT("Source asset object path"))
        .RequiredString(TEXT("destination_path"), TEXT("Full target object path for the duplicate, e.g. /Game/New/Foo.Foo"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("save"),
        TEXT("Save an asset's package to disk and focus it in the Content Browser. Returns {success, asset_path}. "
             "Params: asset_path (string, required, object path), only_if_dirty (bool, optional, default true; pass false to force a write when clean). "
             "Workflow: call once after content_browser/set_metadata or any edit that does not auto-save. "
             "Warning: saving a Material/MaterialInstance or Blueprint recompiles it (can take seconds-minutes); keep only_if_dirty=true so unchanged assets are skipped."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset object path to save"))
        .OptionalBool(TEXT("only_if_dirty"), TEXT("Skip save when the asset is not dirty (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_metadata"),
        TEXT("Set a string metadata tag on a loaded asset via EditorAssetSubsystem and mark the package dirty. Returns {success, asset_path, tag, value}. "
             "Params: asset_path (string, required, object path), tag (string, required, FName-compatible key), value (string, required). "
             "Workflow: call content_browser/save afterward to persist; read it back with content_browser/get_metadata. "
             "Warning: these are editor metadata tags, distinct from cooked Asset Registry tags (which are populated from code and read-only). Does not auto-save."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset object path"))
        .RequiredString(TEXT("tag"), TEXT("Metadata key (FName)"))
        .RequiredString(TEXT("value"), TEXT("Metadata value (string)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_metadata"),
        TEXT("Read a metadata tag from a loaded asset via EditorAssetSubsystem. Returns {success, asset_path, tag, value}. "
             "Params: asset_path (string, required, object path), tag (string, required, FName-compatible key). "
             "Workflow: pair with content_browser/set_metadata to verify a write. "
             "Warning: an absent tag returns the empty string, not an error; loads the asset to read it."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset object path"))
        .RequiredString(TEXT("tag"), TEXT("Metadata key"))
        .Build());

    return Tools;
}

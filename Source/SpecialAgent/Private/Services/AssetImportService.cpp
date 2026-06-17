#include "Services/AssetImportService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Engine/DataTable.h"
#include "Factories/DataTableFactory.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    UAssetImportTask* BuildImportTask(const FString& Filename, const FString& DestinationPath)
    {
        UAssetImportTask* Task = NewObject<UAssetImportTask>();
        Task->Filename = Filename;
        Task->DestinationPath = DestinationPath;
        Task->bAutomated = true;
        Task->bReplaceExisting = true;
        Task->bReplaceExistingSettings = true;
        Task->bSave = true;
        Task->bAsync = false;
        return Task;
    }

    TSharedPtr<FJsonObject> RunImport(const FString& Filename, const FString& DestinationPath)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

        if (!FPaths::FileExists(Filename))
        {
            Result->SetBoolField(TEXT("success"), false);
            Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Source file does not exist: %s"), *Filename));
            return Result;
        }

        UAssetImportTask* Task = BuildImportTask(Filename, DestinationPath);
        TArray<UAssetImportTask*> Tasks{Task};

        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        AssetToolsModule.Get().ImportAssetTasks(Tasks);

        const TArray<UObject*>& Imported = Task->GetObjects();

        TArray<TSharedPtr<FJsonValue>> PathsArray;
        for (const FString& Path : Task->ImportedObjectPaths)
        {
            PathsArray.Add(MakeShared<FJsonValueString>(Path));
        }

        Result->SetBoolField(TEXT("success"), Imported.Num() > 0);
        Result->SetNumberField(TEXT("imported_count"), Imported.Num());
        Result->SetArrayField(TEXT("imported_paths"), PathsArray);
        Result->SetStringField(TEXT("source_file"), Filename);
        Result->SetStringField(TEXT("destination_path"), DestinationPath);

        if (Imported.Num() == 0)
        {
            Result->SetStringField(TEXT("error"), TEXT("Import produced no assets (check source file type and destination path)"));
        }

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: asset_import Filename='%s' Dest='%s' Imported=%d"),
            *Filename, *DestinationPath, Imported.Num());

        return Result;
    }
}

FString FAssetImportService::GetServiceDescription() const
{
    return TEXT("Import FBX / textures / sounds / data tables via IAssetTools::ImportAssetTasks");
}

FMCPResponse FAssetImportService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("import_fbx") ||
        MethodName == TEXT("import_texture") ||
        MethodName == TEXT("import_sound"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString Filename;
        FString DestinationPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("filename"), Filename) ||
            !FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'filename' or 'destination_path'"));
        }

        auto Task = [Filename, DestinationPath]() -> TSharedPtr<FJsonObject>
        {
            return RunImport(Filename, DestinationPath);
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("import_folder"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString SourceFolder;
        FString DestinationPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("source_folder"), SourceFolder) ||
            !FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'source_folder' or 'destination_path'"));
        }

        bool bRecursive = false;
        FMCPJson::ReadBool(Request.Params, TEXT("recursive"), bRecursive);

        // Step 1: validate folder and discover files on the game thread.
        struct FDiscoveryResult
        {
            bool bFolderMissing = false;
            TArray<FString> Files;
        };

        FDiscoveryResult Discovery = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<FDiscoveryResult>(
            [SourceFolder, bRecursive]() -> FDiscoveryResult
            {
                FDiscoveryResult Out;
                IFileManager& FileManager = IFileManager::Get();
                if (!FileManager.DirectoryExists(*SourceFolder))
                {
                    Out.bFolderMissing = true;
                    return Out;
                }

                const FString WildcardPattern = SourceFolder / TEXT("*.*");
                if (bRecursive)
                {
                    FileManager.FindFilesRecursive(Out.Files, *SourceFolder, TEXT("*.*"), /*Files=*/true, /*Directories=*/false);
                }
                else
                {
                    FileManager.FindFiles(Out.Files, *WildcardPattern, /*Files=*/true, /*Directories=*/false);
                    for (FString& File : Out.Files)
                    {
                        File = SourceFolder / File;
                    }
                }
                return Out;
            });

        if (Discovery.bFolderMissing)
        {
            TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
            ErrResult->SetBoolField(TEXT("success"), false);
            ErrResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Source folder does not exist: %s"), *SourceFolder));
            return FMCPResponse::Success(Request.Id, ErrResult);
        }

        const TArray<FString>& FilesToImport = Discovery.Files;
        const int32 Total = FilesToImport.Num();

        if (Total == 0)
        {
            Ctx.SendProgress(1.0, 1.0, TEXT("import_folder: empty folder"));

            TSharedPtr<FJsonObject> EmptyResult = MakeShared<FJsonObject>();
            EmptyResult->SetBoolField(TEXT("success"), true);
            EmptyResult->SetNumberField(TEXT("imported_count"), 0);
            EmptyResult->SetNumberField(TEXT("files_processed"), 0);
            EmptyResult->SetStringField(TEXT("note"), TEXT("No files found in source folder"));
            return FMCPResponse::Success(Request.Id, EmptyResult);
        }

        // Step 2: import files one at a time, emitting progress after each.
        Ctx.SendProgress(0.0, 1.0, FString::Printf(TEXT("import_folder: 0/%d"), Total));

        int32 Succeeded = 0;
        int32 Failed = 0;
        TArray<TSharedPtr<FJsonValue>> PathsArray;
        TArray<TSharedPtr<FJsonValue>> FailedFilesArray;

        for (int32 i = 0; i < Total; ++i)
        {
            const FString File = FilesToImport[i];

            TArray<FString> Imported = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TArray<FString>>(
                [File, DestinationPath]() -> TArray<FString>
                {
                    UAssetImportTask* Task = BuildImportTask(File, DestinationPath);
                    TArray<UAssetImportTask*> Tasks{Task};

                    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
                    AssetToolsModule.Get().ImportAssetTasks(Tasks);

                    UE_LOG(LogTemp, Log, TEXT("SpecialAgent: import_folder file='%s' Dest='%s' Imported=%d"),
                        *File, *DestinationPath, Task->GetObjects().Num());

                    return Task->ImportedObjectPaths;
                });

            if (Imported.Num() > 0)
            {
                ++Succeeded;
                for (const FString& P : Imported)
                {
                    PathsArray.Add(MakeShared<FJsonValueString>(P));
                }
            }
            else
            {
                ++Failed;
                FailedFilesArray.Add(MakeShared<FJsonValueString>(File));
            }

            const double Progress = double(i + 1) / double(Total);
            Ctx.SendProgress(Progress, 1.0,
                FString::Printf(TEXT("import_folder: %d/%d (%s)"),
                    i + 1, Total, *FPaths::GetCleanFilename(File)));
        }

        Ctx.SendProgress(1.0, 1.0,
            FString::Printf(TEXT("import_folder: complete (%d ok, %d failed)"), Succeeded, Failed));

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: import_folder '%s' → %d files, %d succeeded, %d failed"),
            *SourceFolder, Total, Succeeded, Failed);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetNumberField(TEXT("files_processed"), Total);
        Result->SetNumberField(TEXT("imported_count"), Succeeded);
        Result->SetNumberField(TEXT("failed_count"), Failed);
        Result->SetArrayField(TEXT("imported_paths"), PathsArray);
        Result->SetArrayField(TEXT("failed_files"), FailedFilesArray);
        Result->SetStringField(TEXT("source_folder"), SourceFolder);
        Result->SetStringField(TEXT("destination_path"), DestinationPath);

        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("create_data_table_from_csv"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }

        FString CsvPath;
        FString DestinationPath;
        FString AssetName;
        FString RowStructPath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("csv_path"), CsvPath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("asset_name"), AssetName) ||
            !FMCPJson::ReadString(Request.Params, TEXT("row_struct_path"), RowStructPath))
        {
            return InvalidParams(Request.Id, TEXT("Missing one of: csv_path, destination_path, asset_name, row_struct_path"));
        }

        auto Task = [CsvPath, DestinationPath, AssetName, RowStructPath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

            if (!FPaths::FileExists(CsvPath))
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("CSV file does not exist: %s"), *CsvPath));
                return Result;
            }

            FString CsvContent;
            if (!FFileHelper::LoadFileToString(CsvContent, *CsvPath))
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to read CSV: %s"), *CsvPath));
                return Result;
            }

            UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
            if (!RowStruct)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Row struct not found: %s"), *RowStructPath));
                return Result;
            }

            FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");

            UDataTableFactory* Factory = NewObject<UDataTableFactory>();
            Factory->Struct = RowStruct;

            UObject* NewAsset = AssetToolsModule.Get().CreateAsset(AssetName, DestinationPath, UDataTable::StaticClass(), Factory);
            UDataTable* NewTable = Cast<UDataTable>(NewAsset);
            if (!NewTable)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), TEXT("Failed to create UDataTable asset"));
                return Result;
            }

            TArray<FString> Problems = NewTable->CreateTableFromCSVString(CsvContent);

            UPackage* Package = NewTable->GetOutermost();
            Package->MarkPackageDirty();

            TArray<TSharedPtr<FJsonValue>> ProblemsArray;
            for (const FString& P : Problems)
            {
                ProblemsArray.Add(MakeShared<FJsonValueString>(P));
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("asset_path"), NewTable->GetPathName());
            Result->SetNumberField(TEXT("row_count"), NewTable->GetRowMap().Num());
            Result->SetArrayField(TEXT("problems"), ProblemsArray);

            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: create_data_table_from_csv → %s (%d rows, %d problems)"),
                *NewTable->GetPathName(), NewTable->GetRowMap().Num(), Problems.Num());

            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_import_settings_template"))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

        TSharedPtr<FJsonObject> Fbx = MakeShared<FJsonObject>();
        Fbx->SetStringField(TEXT("filename"), TEXT("/absolute/path/to/mesh.fbx"));
        Fbx->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported/Meshes"));

        TSharedPtr<FJsonObject> Texture = MakeShared<FJsonObject>();
        Texture->SetStringField(TEXT("filename"), TEXT("/absolute/path/to/texture.png"));
        Texture->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported/Textures"));

        TSharedPtr<FJsonObject> Sound = MakeShared<FJsonObject>();
        Sound->SetStringField(TEXT("filename"), TEXT("/absolute/path/to/sound.wav"));
        Sound->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported/Sounds"));

        TSharedPtr<FJsonObject> Folder = MakeShared<FJsonObject>();
        Folder->SetStringField(TEXT("source_folder"), TEXT("/absolute/path/to/folder"));
        Folder->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported/Batch"));
        Folder->SetBoolField(TEXT("recursive"), false);

        TSharedPtr<FJsonObject> DataTable = MakeShared<FJsonObject>();
        DataTable->SetStringField(TEXT("csv_path"), TEXT("/absolute/path/to/data.csv"));
        DataTable->SetStringField(TEXT("destination_path"), TEXT("/Game/Data"));
        DataTable->SetStringField(TEXT("asset_name"), TEXT("DT_MyTable"));
        DataTable->SetStringField(TEXT("row_struct_path"), TEXT("/Script/MyGame.MyRowStruct"));

        Result->SetBoolField(TEXT("success"), true);
        Result->SetObjectField(TEXT("import_fbx"), Fbx);
        Result->SetObjectField(TEXT("import_texture"), Texture);
        Result->SetObjectField(TEXT("import_sound"), Sound);
        Result->SetObjectField(TEXT("import_folder"), Folder);
        Result->SetObjectField(TEXT("create_data_table_from_csv"), DataTable);
        Result->SetStringField(TEXT("notes"),
            TEXT("Destination paths are content-browser paths (/Game/...). Source paths are absolute OS paths. All imports use automated=true, replace_existing=true, save=true."));

        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("asset_import"), MethodName);
}

TArray<FMCPToolInfo> FAssetImportService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("import_fbx"),
        TEXT("Import an FBX file as a StaticMesh / SkeletalMesh / Animation. Returns {success, imported_count, imported_paths:[object paths], source_file, destination_path}. "
             "Params: filename (string, required, absolute OS path to a .fbx), destination_path (string, required, virtual content path starting with /Game/, e.g. /Game/Imported/Meshes). "
             "Workflow: inspect imported_paths, then call assets/get_info or assets/get_bounds on a returned path. "
             "Warning: blocking on the game thread; re-import (replace_existing=true) overwrites the existing asset and triggers shader/texture compiles, so re-running is idempotent but expensive. destination_path must be a /Game/ content path, not an OS folder."))
        .RequiredString(TEXT("filename"), TEXT("Absolute OS path to the .fbx file"))
        .RequiredString(TEXT("destination_path"), TEXT("Virtual content destination, e.g. /Game/Imported/Meshes"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("import_texture"),
        TEXT("Import an image file (.png/.jpg/.tga/.exr/...) as a UTexture2D. Returns {success, imported_count, imported_paths:[object paths], source_file, destination_path}. "
             "Params: filename (string, required, absolute OS path to the image), destination_path (string, required, virtual content path /Game/...). "
             "Workflow: after import, load the returned object path and pass the loaded texture to material/set_texture_parameter (a path string alone will not bind). "
             "Warning: blocking; replace_existing=true overwrites and re-runs are idempotent but recompress/recompile. Check the texture's compression_settings/srgb when choosing a sampler type downstream."))
        .RequiredString(TEXT("filename"), TEXT("Absolute OS path to the image file"))
        .RequiredString(TEXT("destination_path"), TEXT("Virtual content destination, e.g. /Game/Imported/Textures"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("import_sound"),
        TEXT("Import a sound file (.wav/.ogg/.flac) as a USoundWave. Returns {success, imported_count, imported_paths:[object paths], source_file, destination_path}. "
             "Params: filename (string, required, absolute OS path to the audio), destination_path (string, required, virtual content path /Game/...). "
             "Workflow: pair with sound/play_2d or sound/play_at_location (passing the imported object path) to verify. "
             "Warning: blocking on the game thread; replace_existing=true overwrites an existing asset of the same name."))
        .RequiredString(TEXT("filename"), TEXT("Absolute OS path to the sound file"))
        .RequiredString(TEXT("destination_path"), TEXT("Virtual content destination, e.g. /Game/Imported/Sounds"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("import_folder"),
        TEXT("Import every file in an OS folder, one at a time, auto-routing each by extension. Returns {success, files_processed, imported_count, failed_count, imported_paths, failed_files, source_folder, destination_path}; emits per-file progress. "
             "Params: source_folder (string, required, absolute OS path), destination_path (string, required, virtual content path /Game/...), recursive (bool, optional, default false; true also imports sub-folders). "
             "Workflow: follow with assets/list on destination_path to confirm. "
             "Warning: blocking on the game thread for the whole batch (slow for large folders); replace_existing=true overwrites same-named assets and triggers compiles. Files an importer cannot handle land in failed_files, not an error."))
        .RequiredString(TEXT("source_folder"), TEXT("Absolute OS path to a folder on disk"))
        .RequiredString(TEXT("destination_path"), TEXT("Virtual content destination, e.g. /Game/Imported/Batch"))
        .OptionalBool(TEXT("recursive"), TEXT("Include files in sub-folders (default false)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("create_data_table_from_csv"),
        TEXT("Create a new UDataTable asset and populate it from a CSV file on disk. Returns {success, asset_path, row_count, problems:[per-row parse messages]}. "
             "Params: csv_path (string, required, absolute OS path to a .csv), destination_path (string, required, virtual content path /Game/...), asset_name (string, required, bare name no extension), row_struct_path (string, required, /Script/Module.StructName of the row struct). "
             "Workflow: inspect problems for rows that failed to parse, then read back with data_table tools. "
             "Warning: row_struct_path must be an existing UScriptStruct whose property/export names match the CSV column headers, or rows silently fail (listed in problems). Creating the asset dirties its package; save to persist."))
        .RequiredString(TEXT("csv_path"), TEXT("Absolute OS path to the .csv file"))
        .RequiredString(TEXT("destination_path"), TEXT("Virtual content destination, e.g. /Game/Data"))
        .RequiredString(TEXT("asset_name"), TEXT("Bare name of the new DataTable asset (no extension)"))
        .RequiredString(TEXT("row_struct_path"), TEXT("Path to the row UScriptStruct, e.g. /Script/MyGame.MyRowStruct"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_import_settings_template"),
        TEXT("Return JSON examples of the parameter shapes for every asset_import tool. Returns {success, import_fbx, import_texture, import_sound, import_folder, create_data_table_from_csv, notes}. Read-only, no side effects. "
             "Params: (none). "
             "Workflow: call once, copy the relevant block, substitute real paths, then call the target import tool. "
             "Warning: examples use placeholder paths - replace them before importing."))
        .Build());

    return Tools;
}

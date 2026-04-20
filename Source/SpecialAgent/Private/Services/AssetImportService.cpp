#include "Services/AssetImportService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

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

FMCPResponse FAssetImportService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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

        auto Task = [SourceFolder, DestinationPath, bRecursive]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

            IFileManager& FileManager = IFileManager::Get();
            if (!FileManager.DirectoryExists(*SourceFolder))
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Source folder does not exist: %s"), *SourceFolder));
                return Result;
            }

            TArray<FString> Files;
            const FString WildcardPattern = SourceFolder / TEXT("*.*");
            if (bRecursive)
            {
                FileManager.FindFilesRecursive(Files, *SourceFolder, TEXT("*.*"), /*Files=*/true, /*Directories=*/false);
            }
            else
            {
                FileManager.FindFiles(Files, *WildcardPattern, /*Files=*/true, /*Directories=*/false);
                for (FString& File : Files)
                {
                    File = SourceFolder / File;
                }
            }

            if (Files.Num() == 0)
            {
                Result->SetBoolField(TEXT("success"), true);
                Result->SetNumberField(TEXT("imported_count"), 0);
                Result->SetNumberField(TEXT("files_processed"), 0);
                Result->SetStringField(TEXT("note"), TEXT("No files found in source folder"));
                return Result;
            }

            TArray<UAssetImportTask*> Tasks;
            Tasks.Reserve(Files.Num());
            for (const FString& File : Files)
            {
                Tasks.Add(BuildImportTask(File, DestinationPath));
            }

            FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
            AssetToolsModule.Get().ImportAssetTasks(Tasks);

            int32 TotalImported = 0;
            TArray<TSharedPtr<FJsonValue>> PathsArray;
            for (UAssetImportTask* T : Tasks)
            {
                TotalImported += T->GetObjects().Num();
                for (const FString& P : T->ImportedObjectPaths)
                {
                    PathsArray.Add(MakeShared<FJsonValueString>(P));
                }
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetNumberField(TEXT("files_processed"), Files.Num());
            Result->SetNumberField(TEXT("imported_count"), TotalImported);
            Result->SetArrayField(TEXT("imported_paths"), PathsArray);
            Result->SetStringField(TEXT("source_folder"), SourceFolder);
            Result->SetStringField(TEXT("destination_path"), DestinationPath);

            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: import_folder '%s' → %d files, %d imported objects"),
                *SourceFolder, Files.Num(), TotalImported);

            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
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
        TEXT("Import an FBX file as a StaticMesh / SkeletalMesh / Animation. Blocking, overwrites existing.\n"
             "Params: filename (string, absolute OS path), destination_path (string, /Game/... content path).\n"
             "Workflow: Inspect the result's imported_paths, then assets/get_info or assets/get_bounds.\n"
             "Warning: Destination path must start with /Game/. Triggered the Phase 0 crash before FMCPGameThreadProcessor fix."))
        .RequiredString(TEXT("filename"), TEXT("Absolute OS path to the .fbx file"))
        .RequiredString(TEXT("destination_path"), TEXT("Content-browser destination (e.g. /Game/Imported/Meshes)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("import_texture"),
        TEXT("Import a texture file (.png/.jpg/.tga/.exr/...) as a UTexture2D. Blocking, overwrites existing.\n"
             "Params: filename (string, absolute OS path), destination_path (string, /Game/... content path).\n"
             "Workflow: After import, material/set_texture_parameter can reference the asset_path."))
        .RequiredString(TEXT("filename"), TEXT("Absolute OS path to the texture file"))
        .RequiredString(TEXT("destination_path"), TEXT("Content-browser destination (e.g. /Game/Imported/Textures)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("import_sound"),
        TEXT("Import a sound file (.wav/.ogg/.flac) as a USoundWave. Blocking, overwrites existing.\n"
             "Params: filename (string, absolute OS path), destination_path (string, /Game/... content path).\n"
             "Workflow: Pair with sound/play_2d or sound/play_at_location to verify."))
        .RequiredString(TEXT("filename"), TEXT("Absolute OS path to the sound file"))
        .RequiredString(TEXT("destination_path"), TEXT("Content-browser destination (e.g. /Game/Imported/Sounds)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("import_folder"),
        TEXT("Import every file in a folder. Extension dictates importer via AssetTools auto-routing.\n"
             "Params: source_folder (string, absolute OS path), destination_path (string, /Game/... path), recursive (bool, optional).\n"
             "Workflow: Returns aggregate imported_paths. Follow with assets/list to confirm.\n"
             "Warning: Large folders are slow and block the game thread for the duration of the import."))
        .RequiredString(TEXT("source_folder"), TEXT("Absolute OS path to a folder on disk"))
        .RequiredString(TEXT("destination_path"), TEXT("Content-browser destination (e.g. /Game/Imported/Batch)"))
        .OptionalBool(TEXT("recursive"), TEXT("Include files in sub-folders (default: false)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("create_data_table_from_csv"),
        TEXT("Create a new UDataTable asset populated from a CSV file on disk.\n"
             "Params: csv_path (string, absolute OS path), destination_path (string, /Game/... path), asset_name (string), row_struct_path (string, /Script/Module.StructName).\n"
             "Workflow: Use data_table/list_rows / get_row to verify. Problems array lists parse issues per row.\n"
             "Warning: row_struct_path must point to an existing UScriptStruct whose fields match CSV columns."))
        .RequiredString(TEXT("csv_path"), TEXT("Absolute OS path to the .csv file"))
        .RequiredString(TEXT("destination_path"), TEXT("Content-browser destination (e.g. /Game/Data)"))
        .RequiredString(TEXT("asset_name"), TEXT("Name of the new DataTable asset (without extension)"))
        .RequiredString(TEXT("row_struct_path"), TEXT("Path to UScriptStruct (e.g. /Script/MyGame.MyRowStruct)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_import_settings_template"),
        TEXT("Return a JSON example of parameter shapes for all asset_import tools. No side effects.\n"
             "Workflow: Call once, copy the relevant block, substitute real paths, then call the target import tool."))
        .Build());

    return Tools;
}

#include "Services/DataTableService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/DataTable.h"
#include "JsonObjectConverter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    UDataTable* LoadTable(const FString& TablePath)
    {
        return LoadObject<UDataTable>(nullptr, *TablePath);
    }

    TSharedPtr<FJsonObject> RowToJson(const UDataTable* Table, FName RowName)
    {
        TSharedPtr<FJsonObject> Out;
        if (!Table || !Table->RowStruct)
        {
            return Out;
        }
        uint8* RowData = Table->FindRowUnchecked(RowName);
        if (!RowData)
        {
            return Out;
        }
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        if (FJsonObjectConverter::UStructToJsonObject(Table->RowStruct, RowData, Json, /*CheckFlags=*/0, /*SkipFlags=*/0))
        {
            return Json;
        }
        return Out;
    }
}

FString FDataTableService::GetServiceDescription() const
{
    return TEXT("Read and write data table rows via reflection");
}

FMCPResponse FDataTableService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("list_tables"))
    {
        auto Task = []() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
            IAssetRegistry& Registry = Module.Get();

            FARFilter Filter;
            Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
            Filter.bRecursiveClasses = true;

            TArray<FAssetData> Tables;
            Registry.GetAssets(Filter, Tables);

            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const FAssetData& Data : Tables)
            {
                TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
                T->SetStringField(TEXT("name"), Data.AssetName.ToString());
                T->SetStringField(TEXT("path"), Data.GetObjectPathString());
                T->SetStringField(TEXT("package_name"), Data.PackageName.ToString());
                Arr.Add(MakeShared<FJsonValueObject>(T));
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetArrayField(TEXT("tables"), Arr);
            Result->SetNumberField(TEXT("count"), Arr.Num());
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: data_table/list_tables → %d"), Arr.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("list_rows"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString TablePath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("table_path"), TablePath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'table_path'"));
        }

        auto Task = [TablePath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UDataTable* Table = LoadTable(TablePath);
            if (!Table)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
                return Result;
            }

            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
            {
                Arr.Add(MakeShared<FJsonValueString>(Row.Key.ToString()));
            }
            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("table_path"), TablePath);
            Result->SetArrayField(TEXT("rows"), Arr);
            Result->SetNumberField(TEXT("count"), Arr.Num());
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: data_table/list_rows '%s' → %d"), *TablePath, Arr.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_row"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString TablePath;
        FString RowName;
        if (!FMCPJson::ReadString(Request.Params, TEXT("table_path"), TablePath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("row_name"), RowName))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'table_path' or 'row_name'"));
        }

        auto Task = [TablePath, RowName]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UDataTable* Table = LoadTable(TablePath);
            if (!Table)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
                return Result;
            }
            TSharedPtr<FJsonObject> Row = RowToJson(Table, FName(*RowName));
            if (!Row.IsValid())
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Row not found or serialization failed: %s"), *RowName));
                return Result;
            }
            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("table_path"), TablePath);
            Result->SetStringField(TEXT("row_name"), RowName);
            Result->SetObjectField(TEXT("row"), Row);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("set_row") || MethodName == TEXT("add_row"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString TablePath;
        FString RowName;
        const TSharedPtr<FJsonObject>* RowObj = nullptr;
        if (!FMCPJson::ReadString(Request.Params, TEXT("table_path"), TablePath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("row_name"), RowName) ||
            !Request.Params->TryGetObjectField(TEXT("row"), RowObj))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'table_path', 'row_name', or 'row' object"));
        }

        TSharedPtr<FJsonObject> RowPayload = *RowObj;
        const bool bAllowCreate = (MethodName == TEXT("add_row"));

        auto Task = [TablePath, RowName, RowPayload, bAllowCreate]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UDataTable* Table = LoadTable(TablePath);
            if (!Table || !Table->RowStruct)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("DataTable or RowStruct missing for: %s"), *TablePath));
                return Result;
            }

            const FName RowKey(*RowName);
            uint8* Existing = Table->FindRowUnchecked(RowKey);
            if (!Existing && !bAllowCreate)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Row does not exist (use add_row): %s"), *RowName));
                return Result;
            }

            // Allocate a temporary struct instance, deserialize JSON into it, then copy into the table.
            const UScriptStruct* RowStruct = Table->RowStruct;
            TArray<uint8> StructStorage;
            StructStorage.AddZeroed(RowStruct->GetStructureSize());
            RowStruct->InitializeStruct(StructStorage.GetData());

            FText FailReason;
            const bool bOk = FJsonObjectConverter::JsonObjectToUStruct(
                RowPayload.ToSharedRef(),
                RowStruct,
                StructStorage.GetData(),
                /*CheckFlags=*/0,
                /*SkipFlags=*/0,
                /*bStrictMode=*/false,
                &FailReason);

            if (!bOk)
            {
                RowStruct->DestroyStruct(StructStorage.GetData());
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("JSON→struct failed: %s"), *FailReason.ToString()));
                return Result;
            }

            Table->AddRow(RowKey, StructStorage.GetData(), RowStruct);
            RowStruct->DestroyStruct(StructStorage.GetData());

            Table->GetOutermost()->MarkPackageDirty();

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("table_path"), TablePath);
            Result->SetStringField(TEXT("row_name"), RowName);
            Result->SetBoolField(TEXT("created"), Existing == nullptr);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: data_table/%s '%s' row='%s' created=%d"),
                bAllowCreate ? TEXT("add_row") : TEXT("set_row"), *TablePath, *RowName, Existing == nullptr ? 1 : 0);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("delete_row"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString TablePath;
        FString RowName;
        if (!FMCPJson::ReadString(Request.Params, TEXT("table_path"), TablePath) ||
            !FMCPJson::ReadString(Request.Params, TEXT("row_name"), RowName))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'table_path' or 'row_name'"));
        }

        auto Task = [TablePath, RowName]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UDataTable* Table = LoadTable(TablePath);
            if (!Table)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
                return Result;
            }
            const FName RowKey(*RowName);
            if (!Table->FindRowUnchecked(RowKey))
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Row not found: %s"), *RowName));
                return Result;
            }
            Table->RemoveRow(RowKey);
            Table->GetOutermost()->MarkPackageDirty();

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("table_path"), TablePath);
            Result->SetStringField(TEXT("row_name"), RowName);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: data_table/delete_row '%s' row='%s'"), *TablePath, *RowName);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_row_struct"))
    {
        if (!Request.Params.IsValid())
        {
            return InvalidParams(Request.Id, TEXT("Missing params object"));
        }
        FString TablePath;
        if (!FMCPJson::ReadString(Request.Params, TEXT("table_path"), TablePath))
        {
            return InvalidParams(Request.Id, TEXT("Missing 'table_path'"));
        }

        auto Task = [TablePath]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            UDataTable* Table = LoadTable(TablePath);
            if (!Table)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
                return Result;
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("table_path"), TablePath);
            if (Table->RowStruct)
            {
                Result->SetStringField(TEXT("row_struct_name"), Table->RowStruct->GetName());
                Result->SetStringField(TEXT("row_struct_path"), Table->RowStruct->GetPathName());

                TArray<TSharedPtr<FJsonValue>> Props;
                for (TFieldIterator<FProperty> It(Table->RowStruct); It; ++It)
                {
                    TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
                    Field->SetStringField(TEXT("name"), It->GetName());
                    Field->SetStringField(TEXT("cpp_type"), It->GetCPPType());
                    Props.Add(MakeShared<FJsonValueObject>(Field));
                }
                Result->SetArrayField(TEXT("fields"), Props);
            }
            else
            {
                Result->SetStringField(TEXT("row_struct_name"), TEXT(""));
            }
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("data_table"), MethodName);
}

TArray<FMCPToolInfo> FDataTableService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("list_tables"),
        TEXT("List all UDataTable assets in the project.\n"
             "Params: (none).\n"
             "Workflow: Follow with list_rows / get_row_struct to inspect a specific table."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_rows"),
        TEXT("List row names of a data table.\n"
             "Params: table_path (string, required, /Game/... asset path).\n"
             "Workflow: pair with data_table/get_row to read individual rows."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable asset path"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_row"),
        TEXT("Read one row as JSON (reflection-based).\n"
             "Params: table_path (string), row_name (string).\n"
             "Workflow: Field names and types come from get_row_struct."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable asset path"))
        .RequiredString(TEXT("row_name"), TEXT("Row key (FName)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_row"),
        TEXT("Update an existing row from JSON (reflection-based).\n"
             "Params: table_path (string), row_name (string), row (object, keys matching struct field names).\n"
             "Workflow: Call content_browser/save afterward to persist.\n"
             "Warning: Fails if row does not exist — use add_row to create."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable asset path"))
        .RequiredString(TEXT("row_name"), TEXT("Row key (FName)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("add_row"),
        TEXT("Add or replace a row in the table with JSON payload.\n"
             "Params: table_path (string), row_name (string), row (object, struct shape).\n"
             "Workflow: Get the expected shape with get_row_struct."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable asset path"))
        .RequiredString(TEXT("row_name"), TEXT("Row key (FName)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("delete_row"),
        TEXT("Remove a row from a DataTable.\n"
             "Params: table_path (string, required), row_name (string, required, FName key).\n"
             "Workflow: pair with content_browser/save to persist; data_table/list_rows to verify.\n"
             "Warning: irreversible without an open undo transaction."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable asset path"))
        .RequiredString(TEXT("row_name"), TEXT("Row key (FName)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_row_struct"),
        TEXT("Return the row struct name, path, and field list (name + cpp_type) for a DataTable.\n"
             "Params: table_path (string).\n"
             "Workflow: Call first when constructing add_row/set_row payloads."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable asset path"))
        .Build());

    return Tools;
}

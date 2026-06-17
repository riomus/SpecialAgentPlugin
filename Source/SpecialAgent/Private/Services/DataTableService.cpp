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
#include "ScopedTransaction.h"

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

            const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Write DataTable Row")));
            Table->Modify();

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
            const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Delete DataTable Row")));
            Table->Modify();

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
        TEXT("List every UDataTable asset in the project via the Asset Registry (no asset loads). Returns {tables[{name, path (object path), package_name}], count}. "
             "Params: (none). Read-only, no side effects. "
             "Workflow: pick a path here, then data_table/get_row_struct to learn the row shape and data_table/list_rows to enumerate rows. "
             "Warning: reads cached registry metadata, so a freshly created table may not appear until the registry has scanned it."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_rows"),
        TEXT("List the row keys (names) of one DataTable. Returns {table_path, rows[string], count}. "
             "Params: table_path (string, required, virtual object path /Game/...; loads the asset). Read-only, no side effects. "
             "Workflow: pair with data_table/get_row to read a row's fields, or use the names as row_name for set_row/delete_row. "
             "Warning: returns an error if table_path does not resolve to a UDataTable."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable object path, e.g. /Game/Data/MyTable.MyTable"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_row"),
        TEXT("Read one DataTable row as a JSON object, serialized from the row struct by reflection. Returns {table_path, row_name, row (object keyed by struct field names)}. "
             "Params: table_path (string, required, virtual object path /Game/...), row_name (string, required, exact row key FName from list_rows). Read-only, no side effects. "
             "Workflow: get the field names/types from data_table/get_row_struct, list keys with data_table/list_rows, then read here. "
             "Warning: returns an error if the table is missing or the row key is not found / fails to serialize."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable object path, e.g. /Game/Data/MyTable.MyTable"))
        .RequiredString(TEXT("row_name"), TEXT("Exact row key (FName) from list_rows"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("set_row"),
        TEXT("Overwrite an EXISTING DataTable row from a JSON object (reflection-based). Returns {table_path, row_name, created (always false here)}. "
             "Params: table_path (string, required, virtual object path /Game/...), row_name (string, required, must already exist), "
             "row (object, required, keys must match the row struct's field names; get them from data_table/get_row_struct). "
             "Workflow: get_row_struct -> build the row object -> set_row -> content_browser/save to persist. "
             "Warning: returns an error if the row does not exist -- use data_table/add_row to create. Only marks the package dirty; it does NOT save to disk, so persist with content_browser/save. Fields whose keys do not match the struct are silently ignored."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable object path, e.g. /Game/Data/MyTable.MyTable"))
        .RequiredString(TEXT("row_name"), TEXT("Existing row key (FName)"))
        .RequiredAny(TEXT("row"), TEXT("Row values as a JSON object keyed by row-struct field names (from get_row_struct)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("add_row"),
        TEXT("Upsert a DataTable row from a JSON object: creates it if absent, replaces it if present (reflection-based). Returns {table_path, row_name, created (true if the row was newly added)}. "
             "Params: table_path (string, required, virtual object path /Game/...), row_name (string, required, FName key), "
             "row (object, required, keys must match the row struct's field names; get them from data_table/get_row_struct). "
             "Workflow: get_row_struct -> build the row object -> add_row -> content_browser/save to persist. "
             "Warning: this is a single-row upsert (UDataTable::AddRow), so it does not clear the rest of the table. Only marks the package dirty -- persist with content_browser/save. Keys not matching the struct are silently dropped; check the Output Log if a row looks empty."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable object path, e.g. /Game/Data/MyTable.MyTable"))
        .RequiredString(TEXT("row_name"), TEXT("Row key (FName)"))
        .RequiredAny(TEXT("row"), TEXT("Row values as a JSON object keyed by row-struct field names (from get_row_struct)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("delete_row"),
        TEXT("Remove a single row from a DataTable (UDataTable::RemoveRow). Returns {table_path, row_name}. "
             "Params: table_path (string, required, virtual object path /Game/...), row_name (string, required, exact FName key). "
             "Workflow: confirm the key with data_table/list_rows, delete here, then content_browser/save to persist; list_rows again to verify. "
             "Warning: returns an error if the row key does not exist. Only marks the package dirty (no in-memory undo here) -- the deletion becomes permanent on the next save."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable object path, e.g. /Game/Data/MyTable.MyTable"))
        .RequiredString(TEXT("row_name"), TEXT("Exact row key (FName) from list_rows"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_row_struct"),
        TEXT("Describe a DataTable's row struct schema. Returns {table_path, row_struct_name, row_struct_path, fields[{name, cpp_type}]}. "
             "Params: table_path (string, required, virtual object path /Game/...). Read-only, no side effects. "
             "Workflow: call this FIRST when building an add_row/set_row 'row' object -- the field names here are exactly the keys the row JSON must use. "
             "Warning: returns an error if the table is missing; if the table has no RowStruct, row_struct_name is empty and fields is omitted."))
        .RequiredString(TEXT("table_path"), TEXT("DataTable object path, e.g. /Game/Data/MyTable.MyTable"))
        .Build());

    return Tools;
}

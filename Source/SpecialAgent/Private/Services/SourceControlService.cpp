#include "Services/SourceControlService.h"
#include "MCPCommon/MCPRequestContext.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
    // Read a string array ("files") from the request params, turning both JSON arrays and
    // single-string inputs into an array of absolute file paths. Returns false if empty.
    static bool ReadFilesArray(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutFiles)
    {
        if (!Params.IsValid()) return false;

        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("files"), Arr))
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                FString S = V->AsString();
                if (!S.IsEmpty())
                {
                    OutFiles.Add(USourceControlHelpers::AbsoluteFilenames({S})[0]);
                }
            }
        }
        else
        {
            FString Single;
            if (Params->TryGetStringField(TEXT("file"), Single) && !Single.IsEmpty())
            {
                OutFiles.Add(USourceControlHelpers::AbsoluteFilenames({Single})[0]);
            }
        }
        return OutFiles.Num() > 0;
    }

    static TSharedPtr<FJsonObject> SerializeState(const ISourceControlState& State)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("file"), State.GetFilename());
        Obj->SetBoolField(TEXT("is_source_controlled"), State.IsSourceControlled());
        Obj->SetBoolField(TEXT("is_checked_out"), State.IsCheckedOut());
        Obj->SetBoolField(TEXT("is_checked_out_other"), State.IsCheckedOutOther());
        Obj->SetBoolField(TEXT("is_added"), State.IsAdded());
        Obj->SetBoolField(TEXT("is_deleted"), State.IsDeleted());
        Obj->SetBoolField(TEXT("is_modified"), State.IsModified());
        Obj->SetBoolField(TEXT("is_ignored"), State.IsIgnored());
        Obj->SetBoolField(TEXT("can_checkout"), State.CanCheckout());
        return Obj;
    }
}

FString FSourceControlService::GetServiceDescription() const
{
    return TEXT("Source control (revision control) - status, check-out, revert, submit files");
}

FMCPResponse FSourceControlService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("get_status"))     return HandleGetStatus(Request);
    if (MethodName == TEXT("check_out"))      return HandleCheckOut(Request);
    if (MethodName == TEXT("revert"))         return HandleRevert(Request);
    if (MethodName == TEXT("submit"))         return HandleSubmit(Request);
    if (MethodName == TEXT("list_modified"))  return HandleListModified(Request);

    return MethodNotFound(Request.Id, TEXT("source_control"), MethodName);
}

FMCPResponse FSourceControlService::HandleGetStatus(const FMCPRequest& Request)
{
    TArray<FString> Files;
    if (!ReadFilesArray(Request.Params, Files))
    {
        return InvalidParams(Request.Id, TEXT("Provide 'files' (string[]) or 'file' (string)"));
    }

    auto Task = [Files]() -> TSharedPtr<FJsonObject>
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        if (!Module.IsEnabled())
        {
            return FMCPJson::MakeError(TEXT("Source control is not enabled"));
        }

        ISourceControlProvider& Provider = Module.GetProvider();
        TArray<FSourceControlStateRef> States;
        const ECommandResult::Type Result =
            Provider.GetState(Files, States, EStateCacheUsage::ForceUpdate);

        if (Result != ECommandResult::Succeeded)
        {
            return FMCPJson::MakeError(TEXT("Provider.GetState failed"));
        }

        TArray<TSharedPtr<FJsonValue>> StatesArr;
        for (const FSourceControlStateRef& State : States)
        {
            StatesArr.Add(MakeShared<FJsonValueObject>(SerializeState(*State)));
        }

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetArrayField(TEXT("states"), StatesArr);
        Out->SetNumberField(TEXT("count"), StatesArr.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: source_control/get_status returned %d entries"), StatesArr.Num());
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSourceControlService::HandleCheckOut(const FMCPRequest& Request)
{
    TArray<FString> Files;
    if (!ReadFilesArray(Request.Params, Files))
    {
        return InvalidParams(Request.Id, TEXT("Provide 'files' (string[]) or 'file' (string)"));
    }

    auto Task = [Files]() -> TSharedPtr<FJsonObject>
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        if (!Module.IsEnabled()) return FMCPJson::MakeError(TEXT("Source control is not enabled"));

        ISourceControlProvider& Provider = Module.GetProvider();
        const ECommandResult::Type Result =
            Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Files);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("success"), Result == ECommandResult::Succeeded);
        Out->SetNumberField(TEXT("count"), Files.Num());
        if (Result != ECommandResult::Succeeded)
        {
            Out->SetStringField(TEXT("error"), TEXT("FCheckOut failed"));
        }
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: source_control/check_out files=%d result=%d"),
            Files.Num(), (int32)Result);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSourceControlService::HandleRevert(const FMCPRequest& Request)
{
    TArray<FString> Files;
    if (!ReadFilesArray(Request.Params, Files))
    {
        return InvalidParams(Request.Id, TEXT("Provide 'files' (string[]) or 'file' (string)"));
    }

    auto Task = [Files]() -> TSharedPtr<FJsonObject>
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        if (!Module.IsEnabled()) return FMCPJson::MakeError(TEXT("Source control is not enabled"));

        ISourceControlProvider& Provider = Module.GetProvider();
        const ECommandResult::Type Result =
            Provider.Execute(ISourceControlOperation::Create<FRevert>(), Files);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("success"), Result == ECommandResult::Succeeded);
        Out->SetNumberField(TEXT("count"), Files.Num());
        if (Result != ECommandResult::Succeeded)
        {
            Out->SetStringField(TEXT("error"), TEXT("FRevert failed"));
        }
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: source_control/revert files=%d result=%d"),
            Files.Num(), (int32)Result);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSourceControlService::HandleSubmit(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    TArray<FString> Files;
    if (!ReadFilesArray(Request.Params, Files))
    {
        return InvalidParams(Request.Id, TEXT("Provide 'files' (string[]) or 'file' (string)"));
    }

    FString Description;
    if (!FMCPJson::ReadString(Request.Params, TEXT("description"), Description) || Description.IsEmpty())
    {
        return InvalidParams(Request.Id, TEXT("Missing 'description'"));
    }

    auto Task = [Files, Description]() -> TSharedPtr<FJsonObject>
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        if (!Module.IsEnabled()) return FMCPJson::MakeError(TEXT("Source control is not enabled"));

        ISourceControlProvider& Provider = Module.GetProvider();
        TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn = ISourceControlOperation::Create<FCheckIn>();
        CheckIn->SetDescription(FText::FromString(Description));

        const ECommandResult::Type Result = Provider.Execute(CheckIn, Files);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("success"), Result == ECommandResult::Succeeded);
        Out->SetNumberField(TEXT("count"), Files.Num());
        Out->SetStringField(TEXT("description"), Description);
        if (Result != ECommandResult::Succeeded)
        {
            Out->SetStringField(TEXT("error"), TEXT("FCheckIn failed"));
        }
        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: source_control/submit files=%d result=%d"),
            Files.Num(), (int32)Result);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FSourceControlService::HandleListModified(const FMCPRequest& Request)
{
    FString SearchPath;
    FMCPJson::ReadString(Request.Params, TEXT("path"), SearchPath);

    auto Task = [SearchPath]() -> TSharedPtr<FJsonObject>
    {
        ISourceControlModule& Module = ISourceControlModule::Get();
        if (!Module.IsEnabled())
        {
            return FMCPJson::MakeError(TEXT("Source control is not enabled"));
        }

        ISourceControlProvider& Provider = Module.GetProvider();

        // Default search root is the project's Content directory.
        FString Root = SearchPath.IsEmpty() ? FPaths::ProjectContentDir() : SearchPath;
        Root = FPaths::ConvertRelativePathToFull(Root);

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.*"), /*bFiles=*/true, /*bDirs=*/false);

        if (Files.Num() == 0)
        {
            TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
            Out->SetArrayField(TEXT("files"), {});
            Out->SetNumberField(TEXT("count"), 0);
            return Out;
        }

        // Run an UpdateStatus so the provider caches are fresh before querying.
        Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files);

        TArray<FSourceControlStateRef> States;
        Provider.GetState(Files, States, EStateCacheUsage::Use);

        TArray<TSharedPtr<FJsonValue>> ModifiedArr;
        for (const FSourceControlStateRef& State : States)
        {
            if (State->IsModified() || State->IsAdded() || State->IsDeleted() || State->IsCheckedOut())
            {
                ModifiedArr.Add(MakeShared<FJsonValueObject>(SerializeState(*State)));
            }
        }

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetArrayField(TEXT("files"), ModifiedArr);
        Out->SetNumberField(TEXT("count"), ModifiedArr.Num());
        Out->SetStringField(TEXT("search_path"), Root);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: source_control/list_modified scanned=%d modified=%d"),
            Files.Num(), ModifiedArr.Num());
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FSourceControlService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("get_status"),
            TEXT("Query revision-control state for one or more files, forcing a fresh status update from the active provider. "
                 "Returns {success, count, states:[{file, is_source_controlled, is_checked_out, is_checked_out_other, is_added, is_deleted, is_modified, is_ignored, can_checkout}]}. "
                 "Params: files (string[] of OS file paths, absolute or project-relative) OR file (single string); at least one is required. "
                 "Workflow: call before source_control/check_out to verify files are controlled and can_checkout is true. "
                 "Warning: read-only; takes OS file paths (e.g. Content/Maps/M.umap), NOT /Game/ virtual paths, and errors if no provider (Perforce, Git, etc.) is configured."))
        .OptionalArrayOfString(TEXT("files"), TEXT("Array of OS file paths (absolute or project-relative)."))
        .OptionalString       (TEXT("file"),  TEXT("Single OS file path; accepted as an alternative to 'files'."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("check_out"),
            TEXT("Check out files for editing through the active provider (runs an FCheckOut operation). "
                 "Returns {success, count, error?} where success reflects whether the provider command succeeded. "
                 "Params: files (string[] of OS file paths) OR file (single string); at least one is required. "
                 "Workflow: call source_control/get_status first to confirm can_checkout is true. "
                 "Warning: side-effecting; takes OS file paths not /Game/ paths, and may report success even on providers like Git that have no checkout concept, so always inspect 'success'."))
        .OptionalArrayOfString(TEXT("files"), TEXT("Array of OS file paths to check out."))
        .OptionalString       (TEXT("file"),  TEXT("Single OS file path to check out."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("revert"),
            TEXT("Discard local changes to files, restoring them to the depot/HEAD revision (runs an FRevert operation). "
                 "Returns {success, count, error?}. "
                 "Params: files (string[] of OS file paths) OR file (single string); at least one is required. "
                 "Workflow: call source_control/get_status first to see what is modified; use to undo unintended edits. "
                 "Warning: DESTRUCTIVE and not undoable - local edits are permanently lost, and open packages may need reloading afterwards."))
        .OptionalArrayOfString(TEXT("files"), TEXT("Array of OS file paths to revert."))
        .OptionalString       (TEXT("file"),  TEXT("Single OS file path to revert."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("submit"),
            TEXT("Submit (check in) files with a change description through the active provider (runs an FCheckIn operation). "
                 "Returns {success, count, description, error?}. "
                 "Params: files (string[] of OS file paths) OR file (single string), at least one required; description (string, required commit/change message). "
                 "Workflow: run source_control/list_modified beforehand to confirm exactly what will be submitted. "
                 "Warning: side-effecting and effectively irreversible on centralized providers like Perforce; takes OS file paths, not /Game/ paths."))
        .OptionalArrayOfString(TEXT("files"),       TEXT("Array of OS file paths to submit."))
        .OptionalString       (TEXT("file"),        TEXT("Single OS file path to submit."))
        .RequiredString       (TEXT("description"), TEXT("Commit / change description (required)."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("list_modified"),
            TEXT("Recursively scan a directory and list files that are modified, added, deleted, or checked out (runs FUpdateStatus, then reads cached state). "
                 "Returns {success, count, search_path, files:[{file, is_source_controlled, is_checked_out, is_added, is_deleted, is_modified, ...}]}. "
                 "Params: path (string, optional OS directory root, absolute or project-relative; defaults to the project Content/ folder). "
                 "Workflow: call before source_control/submit to review exactly what is pending. "
                 "Warning: read-only but can be slow - it walks every file under the root; for large projects narrow 'path' to a subfolder."))
        .OptionalString(TEXT("path"), TEXT("OS directory root to scan; defaults to the project's Content folder."))
        .Build());

    return Tools;
}

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
            TEXT("Query revision control state for one or more files. Forces a refresh via the active provider. "
                 "Params: files (string[] absolute or project-relative paths) OR file (single string). "
                 "Workflow: call before check_out to verify files are source-controlled and available. "
                 "Warning: requires a configured source control provider (Perforce, Git, etc.)."))
        .OptionalArrayOfString(TEXT("files"), TEXT("Array of file paths (absolute or project-relative)"))
        .OptionalString       (TEXT("file"),  TEXT("Single file path; accepted as an alternative to 'files'"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("check_out"),
            TEXT("Check out files for editing via the source control provider (FCheckOut). "
                 "Params: files (string[]) or file (string). "
                 "Workflow: call source_control/get_status first to confirm CanCheckout. "
                 "Warning: may silently fail on providers like Git that do not require checkout; inspect the returned 'success'."))
        .OptionalArrayOfString(TEXT("files"), TEXT("Array of file paths to check out"))
        .OptionalString       (TEXT("file"),  TEXT("Single file path to check out"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("revert"),
            TEXT("Revert local changes to files via the source control provider (FRevert). "
                 "Params: files (string[]) or file (string). "
                 "Workflow: call after unintended edits to discard local modifications. "
                 "Warning: destructive - local edits are lost and reload may be needed for open packages."))
        .OptionalArrayOfString(TEXT("files"), TEXT("Array of file paths to revert"))
        .OptionalString       (TEXT("file"),  TEXT("Single file path to revert"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("submit"),
            TEXT("Submit (check-in) files with a description via the source control provider (FCheckIn). "
                 "Params: files (string[]) or file (string); description (string, required commit message). "
                 "Workflow: run source_control/list_modified beforehand to confirm what will be submitted. "
                 "Warning: irreversible on centralized providers like Perforce."))
        .OptionalArrayOfString(TEXT("files"),       TEXT("Array of file paths to submit"))
        .OptionalString       (TEXT("file"),        TEXT("Single file path to submit"))
        .RequiredString       (TEXT("description"), TEXT("Commit / change description"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("list_modified"),
            TEXT("Enumerate modified, added, deleted, or checked-out files within a directory. Runs FUpdateStatus, then queries state. "
                 "Params: path (string, optional absolute or project-relative root; defaults to Content dir). "
                 "Workflow: call before submit to review the pending changes. "
                 "Warning: may scan many files; for large projects, narrow the 'path'."))
        .OptionalString(TEXT("path"), TEXT("Root directory to scan; defaults to the project's Content folder"))
        .Build());

    return Tools;
}

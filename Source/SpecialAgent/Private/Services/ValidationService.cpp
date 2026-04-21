#include "Services/ValidationService.h"
#include "MCPCommon/MCPRequestContext.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "IMessageLogListing.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "Misc/DataValidation.h"
#include "Modules/ModuleManager.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace
{
    /**
     * Run UEditorValidatorSubsystem::ValidateAssetsWithSettings on the provided
     * AssetData list and shape the results into the JSON response used by
     * existing MCP clients: {success, validated_count, issue_count, errors[], warnings[]}.
     *
     * We also emit per-asset details (same shape the old handler used) so clients
     * that already consumed that get continuity.
     */
    TSharedPtr<FJsonObject> RunValidatorSubsystem(const TArray<FAssetData>& AssetDataArray)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

        UEditorValidatorSubsystem* ValidatorSubsystem = GEditor ?
            GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>() : nullptr;
        if (!ValidatorSubsystem)
        {
            Result->SetBoolField(TEXT("success"), false);
            Result->SetStringField(TEXT("error"), TEXT("UEditorValidatorSubsystem not available"));
            return Result;
        }

        FValidateAssetsSettings Settings;
        Settings.ValidationUsecase = EDataValidationUsecase::Manual;
        Settings.bCollectPerAssetDetails = true;
        Settings.bSilent = true;
        Settings.bShowIfNoFailures = false;

        FValidateAssetsResults Results;
        const int32 Issues = ValidatorSubsystem->ValidateAssetsWithSettings(AssetDataArray, Settings, Results);

        TArray<TSharedPtr<FJsonValue>> ErrorArr;
        TArray<TSharedPtr<FJsonValue>> WarningArr;
        TArray<TSharedPtr<FJsonValue>> PerAsset;

        for (const TPair<FString, FValidateAssetsDetails>& Entry : Results.AssetsDetails)
        {
            const FValidateAssetsDetails& Details = Entry.Value;

            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("asset"), Entry.Key);
            Item->SetStringField(TEXT("package"), Details.PackageName.ToString());

            FString ResultStr;
            switch (Details.Result)
            {
                case EDataValidationResult::Valid:        ResultStr = TEXT("valid"); break;
                case EDataValidationResult::Invalid:      ResultStr = TEXT("invalid"); break;
                case EDataValidationResult::NotValidated: ResultStr = TEXT("not_validated"); break;
                default:                                  ResultStr = TEXT("unknown"); break;
            }
            Item->SetStringField(TEXT("result"), ResultStr);
            Item->SetNumberField(TEXT("num_errors"), Details.ValidationErrors.Num());
            Item->SetNumberField(TEXT("num_warnings"), Details.ValidationWarnings.Num());

            TArray<TSharedPtr<FJsonValue>> ItemErrs;
            for (const FText& E : Details.ValidationErrors)
            {
                const FString S = E.ToString();
                ItemErrs.Add(MakeShared<FJsonValueString>(S));
                ErrorArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *Entry.Key, *S)));
            }
            Item->SetArrayField(TEXT("errors"), ItemErrs);

            TArray<TSharedPtr<FJsonValue>> ItemWarns;
            for (const FText& W : Details.ValidationWarnings)
            {
                const FString S = W.ToString();
                ItemWarns.Add(MakeShared<FJsonValueString>(S));
                WarningArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *Entry.Key, *S)));
            }
            Item->SetArrayField(TEXT("warnings"), ItemWarns);

            PerAsset.Add(MakeShared<FJsonValueObject>(Item));
        }

        Result->SetBoolField(TEXT("success"), true);
        Result->SetNumberField(TEXT("validated_count"), Results.NumChecked);
        Result->SetNumberField(TEXT("issue_count"), Issues);
        Result->SetNumberField(TEXT("num_requested"), Results.NumRequested);
        Result->SetNumberField(TEXT("num_valid"), Results.NumValid);
        Result->SetNumberField(TEXT("num_invalid"), Results.NumInvalid);
        Result->SetNumberField(TEXT("num_skipped"), Results.NumSkipped);
        Result->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
        Result->SetNumberField(TEXT("num_unable_to_validate"), Results.NumUnableToValidate);
        Result->SetArrayField(TEXT("errors"), ErrorArr);
        Result->SetArrayField(TEXT("warnings"), WarningArr);
        Result->SetArrayField(TEXT("assets"), PerAsset);

        // Backward-compat keys from the old shape.
        Result->SetNumberField(TEXT("total"), Results.NumRequested);
        Result->SetNumberField(TEXT("valid"), Results.NumValid);
        Result->SetNumberField(TEXT("invalid"), Results.NumInvalid);
        Result->SetNumberField(TEXT("not_validated"), Results.NumUnableToValidate);

        return Result;
    }
}

FString FValidationService::GetServiceDescription() const
{
    return TEXT("Asset and level validation via UEditorValidatorSubsystem");
}

FMCPResponse FValidationService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("validate_selected"))
    {
        auto Task = []() -> TSharedPtr<FJsonObject>
        {
            TArray<FAssetData> AssetDataArray;

            // Content-browser-selected assets.
            FContentBrowserModule& CBModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
            CBModule.Get().GetSelectedAssets(AssetDataArray);

            // Selected actors: include the actor's class asset (Blueprint-authored actor classes),
            // which is what UEditorValidatorSubsystem can actually validate.
            IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
            if (GEditor)
            {
                if (USelection* Sel = GEditor->GetSelectedActors())
                {
                    TArray<AActor*> Actors;
                    Sel->GetSelectedObjects<AActor>(Actors);
                    TSet<UClass*> SeenClasses;
                    for (AActor* A : Actors)
                    {
                        if (!A) continue;
                        UClass* Cls = A->GetClass();
                        if (!Cls || SeenClasses.Contains(Cls)) continue;
                        SeenClasses.Add(Cls);

                        if (UPackage* Pkg = Cls->GetOutermost())
                        {
                            TArray<FAssetData> ClassAssets;
                            Registry.GetAssetsByPackageName(Pkg->GetFName(), ClassAssets);
                            AssetDataArray.Append(ClassAssets);
                        }
                    }
                }
            }

            TSharedPtr<FJsonObject> Result = RunValidatorSubsystem(AssetDataArray);
            Result->SetStringField(TEXT("source"), TEXT("selection"));
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: validation/validate_selected -> %d asset(s)"),
                AssetDataArray.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("validate_level"))
    {
        auto Task = []() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (!World)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), TEXT("No editor world"));
                return Result;
            }

            IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
            const FName LevelPackage = World->GetOutermost()->GetFName();

            // Start with the level itself.
            TArray<FAssetData> AssetDataArray;
            Registry.GetAssetsByPackageName(LevelPackage, AssetDataArray);

            // Walk package dependencies, capped so very large levels don't stall validation.
            TArray<FName> Deps;
            Registry.GetDependencies(LevelPackage, Deps, UE::AssetRegistry::EDependencyCategory::Package);

            int32 DepsScanned = 0;
            for (const FName& DepPkg : Deps)
            {
                const FString PkgStr = DepPkg.ToString();
                if (PkgStr.StartsWith(TEXT("/Script/")) || PkgStr.StartsWith(TEXT("/Engine/")))
                {
                    continue;
                }
                TArray<FAssetData> DepAssets;
                Registry.GetAssetsByPackageName(DepPkg, DepAssets);
                AssetDataArray.Append(DepAssets);
                ++DepsScanned;
                if (DepsScanned >= 256)
                {
                    break;
                }
            }

            TSharedPtr<FJsonObject> Sub = RunValidatorSubsystem(AssetDataArray);
            Result->SetStringField(TEXT("level_package"), LevelPackage.ToString());
            Result->Values.Append(Sub->Values);
            Result->SetNumberField(TEXT("level_deps_scanned"), DepsScanned);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: validation/validate_level '%s' -> %d asset(s)"),
                *LevelPackage.ToString(), AssetDataArray.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("list_errors"))
    {
        auto Task = []() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> MessageArr;

            const TArray<FName> LogsToCheck{
                FName(TEXT("AssetCheck")),
                FName(TEXT("MapCheck")),
                FName(TEXT("AssetTools")),
                FName(TEXT("LoadErrors")),
            };

            constexpr int32 MaxMessagesPerLog = 32;

            FMessageLogModule* MessageLogModule = FModuleManager::LoadModulePtr<FMessageLogModule>(TEXT("MessageLog"));

            int32 TotalMessages = 0;
            int32 TotalErrors = 0;
            int32 TotalWarnings = 0;
            for (const FName& LogName : LogsToCheck)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("log"), LogName.ToString());

                FMessageLog Log(LogName);
                const int32 InfoCount = Log.NumMessages(EMessageSeverity::Info);
                const int32 WarningCount = Log.NumMessages(EMessageSeverity::Warning);
                const int32 ErrorCount = Log.NumMessages(EMessageSeverity::Error);
                Entry->SetNumberField(TEXT("num_messages"), InfoCount);
                Entry->SetNumberField(TEXT("num_warnings"), WarningCount);
                Entry->SetNumberField(TEXT("num_errors"), ErrorCount);

                TotalMessages += InfoCount;
                TotalWarnings += WarningCount;
                TotalErrors += ErrorCount;

                // If the listing UI is registered, pull the most recent filtered messages so the
                // client sees actual text, not just counts. This is best-effort; logs that were
                // never registered (rare for the four above) will report empty messages.
                TArray<TSharedPtr<FJsonValue>> RecentMessages;
                if (MessageLogModule && MessageLogModule->IsRegisteredLogListing(LogName))
                {
                    TSharedRef<IMessageLogListing> Listing = MessageLogModule->GetLogListing(LogName);
                    const TArray<TSharedRef<FTokenizedMessage>>& Filtered = Listing->GetFilteredMessages();
                    const int32 StartIdx = FMath::Max(0, Filtered.Num() - MaxMessagesPerLog);
                    for (int32 i = StartIdx; i < Filtered.Num(); ++i)
                    {
                        const TSharedRef<FTokenizedMessage>& Msg = Filtered[i];
                        TSharedPtr<FJsonObject> MsgJson = MakeShared<FJsonObject>();
                        FString SeverityStr;
                        switch (Msg->GetSeverity())
                        {
                            case EMessageSeverity::Error:              SeverityStr = TEXT("error"); break;
                            case EMessageSeverity::PerformanceWarning: SeverityStr = TEXT("performance_warning"); break;
                            case EMessageSeverity::Warning:            SeverityStr = TEXT("warning"); break;
                            case EMessageSeverity::Info:               SeverityStr = TEXT("info"); break;
                            default:                                   SeverityStr = TEXT("unknown"); break;
                        }
                        MsgJson->SetStringField(TEXT("severity"), SeverityStr);
                        MsgJson->SetStringField(TEXT("text"), Msg->ToText().ToString());
                        RecentMessages.Add(MakeShared<FJsonValueObject>(MsgJson));
                    }
                }
                Entry->SetArrayField(TEXT("recent_messages"), RecentMessages);

                MessageArr.Add(MakeShared<FJsonValueObject>(Entry));
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetArrayField(TEXT("logs"), MessageArr);
            Result->SetNumberField(TEXT("total"), TotalMessages);
            Result->SetNumberField(TEXT("total_errors"), TotalErrors);
            Result->SetNumberField(TEXT("total_warnings"), TotalWarnings);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: validation/list_errors -> %d log(s) surveyed, %d errors, %d warnings"),
                LogsToCheck.Num(), TotalErrors, TotalWarnings);
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("validation"), MethodName);
}

TArray<FMCPToolInfo> FValidationService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("validate_selected"),
        TEXT("Run UEditorValidatorSubsystem on assets selected in the Content Browser plus\n"
             "the class assets of any selected actors. Uses project-configured validators\n"
             "(IsDataValid overrides, asset-naming, custom EditorValidatorBase).\n"
             "Returns validated_count, issue_count, errors[], warnings[] plus per-asset detail.\n"
             "Workflow: utility/focus_asset_in_browser or actor selection first, then call this."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("validate_level"),
        TEXT("Validate the current editor level and its on-disk package dependencies via\n"
             "UEditorValidatorSubsystem. Capped at 256 dep packages; Engine/script excluded.\n"
             "Workflow: Use before checking in a level; follow with content_browser/save for any fixes."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_errors"),
        TEXT("Survey AssetCheck, MapCheck, AssetTools, and LoadErrors message logs. Returns\n"
             "counts per severity and up to 32 recent tokenized messages per log where the\n"
             "log listing UI is registered. No params."))
        .Build());

    return Tools;
}

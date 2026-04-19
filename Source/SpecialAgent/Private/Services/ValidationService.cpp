#include "Services/ValidationService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "IContentBrowserSingleton.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/DataValidation.h"
#include "UObject/Object.h"

namespace
{
    // Runs UObject::IsDataValid(FDataValidationContext&) — always available in CoreUObject,
    // independent of the DataValidation editor plugin.
    TSharedPtr<FJsonObject> ValidateAssetList(const TArray<UObject*>& Assets)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        int32 NumValid = 0;
        int32 NumInvalid = 0;
        int32 NumNotValidated = 0;

        TArray<TSharedPtr<FJsonValue>> PerAsset;
        for (UObject* Obj : Assets)
        {
            if (!Obj)
            {
                continue;
            }
            FDataValidationContext Context(/*bWasAssetLoadedForValidation=*/false,
                                           EDataValidationUsecase::Script,
                                           /*AssociatedObjects=*/TConstArrayView<FAssetData>());
            const UObject* ConstObj = Obj;
            EDataValidationResult ValidationResult = ConstObj->IsDataValid(Context);

            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("asset"), Obj->GetPathName());

            FString ResultStr;
            switch (ValidationResult)
            {
                case EDataValidationResult::Valid:       ResultStr = TEXT("valid"); ++NumValid; break;
                case EDataValidationResult::Invalid:     ResultStr = TEXT("invalid"); ++NumInvalid; break;
                case EDataValidationResult::NotValidated: ResultStr = TEXT("not_validated"); ++NumNotValidated; break;
                default: ResultStr = TEXT("unknown"); break;
            }
            Item->SetStringField(TEXT("result"), ResultStr);
            Item->SetNumberField(TEXT("num_errors"), Context.GetNumErrors());
            Item->SetNumberField(TEXT("num_warnings"), Context.GetNumWarnings());

            TArray<FText> Warnings, Errors;
            Context.SplitIssues(Warnings, Errors);

            TArray<TSharedPtr<FJsonValue>> ErrArr;
            for (const FText& E : Errors)
            {
                ErrArr.Add(MakeShared<FJsonValueString>(E.ToString()));
            }
            Item->SetArrayField(TEXT("errors"), ErrArr);

            TArray<TSharedPtr<FJsonValue>> WarnArr;
            for (const FText& W : Warnings)
            {
                WarnArr.Add(MakeShared<FJsonValueString>(W.ToString()));
            }
            Item->SetArrayField(TEXT("warnings"), WarnArr);

            PerAsset.Add(MakeShared<FJsonValueObject>(Item));
        }

        Result->SetBoolField(TEXT("success"), true);
        Result->SetNumberField(TEXT("total"), Assets.Num());
        Result->SetNumberField(TEXT("valid"), NumValid);
        Result->SetNumberField(TEXT("invalid"), NumInvalid);
        Result->SetNumberField(TEXT("not_validated"), NumNotValidated);
        Result->SetArrayField(TEXT("assets"), PerAsset);
        return Result;
    }
}

FString FValidationService::GetServiceDescription() const
{
    return TEXT("Asset and level validation via UObject::IsDataValid");
}

FMCPResponse FValidationService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
    if (MethodName == TEXT("validate_selected"))
    {
        auto Task = []() -> TSharedPtr<FJsonObject>
        {
            // Collect assets currently selected in the Content Browser.
            FContentBrowserModule& CBModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
            TArray<FAssetData> Selected;
            CBModule.Get().GetSelectedAssets(Selected);

            TArray<UObject*> Loaded;
            for (const FAssetData& Data : Selected)
            {
                if (UObject* Obj = Data.GetAsset())
                {
                    Loaded.Add(Obj);
                }
            }

            TSharedPtr<FJsonObject> Result = ValidateAssetList(Loaded);
            Result->SetStringField(TEXT("source"), TEXT("content_browser_selection"));
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: validation/validate_selected → %d assets"), Loaded.Num());
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

            UWorld* World = nullptr;
            if (GEditor)
            {
                World = GEditor->GetEditorWorldContext().World();
            }
            if (!World)
            {
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("error"), TEXT("No editor world"));
                return Result;
            }

            // Gather all assets referenced by actors in the persistent level.
            IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
            const FName LevelPackage = World->GetOutermost()->GetFName();

            TArray<FName> Deps;
            Registry.GetDependencies(LevelPackage, Deps, UE::AssetRegistry::EDependencyCategory::Package);

            TArray<UObject*> Loaded;
            // Also validate the level itself.
            Loaded.Add(World);

            int32 LoadedFromDeps = 0;
            for (const FName& DepPkg : Deps)
            {
                const FString PkgStr = DepPkg.ToString();
                if (PkgStr.StartsWith(TEXT("/Script/")) || PkgStr.StartsWith(TEXT("/Engine/")))
                {
                    continue;
                }
                if (UObject* Obj = UEditorAssetLibrary::LoadAsset(PkgStr))
                {
                    Loaded.Add(Obj);
                    ++LoadedFromDeps;
                    if (LoadedFromDeps >= 256)
                    {
                        break;
                    }
                }
            }

            TSharedPtr<FJsonObject> Sub = ValidateAssetList(Loaded);
            // Copy sub result fields into Result.
            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("level_package"), LevelPackage.ToString());
            Result->Values.Append(Sub->Values);
            Result->SetNumberField(TEXT("level_deps_scanned"), LoadedFromDeps);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: validation/validate_level '%s' → %d objs"),
                *LevelPackage.ToString(), Loaded.Num());
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

            // Query the Map Check and Asset Check message logs. FMessageLog itself lives in
            // Core; the MessageLog developer module hosts the UI but isn't required to count
            // messages in existing logs.
            const TArray<FName> LogsToCheck{
                FName(TEXT("MapCheck")),
                FName(TEXT("AssetCheck")),
                FName(TEXT("AssetTools")),
                FName(TEXT("LoadErrors")),
            };

            int32 TotalMessages = 0;
            for (const FName& LogName : LogsToCheck)
            {
                FMessageLog Log(LogName);
                // FMessageLog doesn't expose an enumeration API directly on the public surface;
                // the message counts are the best approachable signal.
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("log"), LogName.ToString());
                Entry->SetNumberField(TEXT("num_messages"), Log.NumMessages(EMessageSeverity::Info));
                Entry->SetNumberField(TEXT("num_warnings"), Log.NumMessages(EMessageSeverity::Warning));
                Entry->SetNumberField(TEXT("num_errors"), Log.NumMessages(EMessageSeverity::Error));
                MessageArr.Add(MakeShared<FJsonValueObject>(Entry));
                TotalMessages += Log.NumMessages(EMessageSeverity::Info);
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetArrayField(TEXT("logs"), MessageArr);
            Result->SetNumberField(TEXT("total"), TotalMessages);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: validation/list_errors → %d logs surveyed"), LogsToCheck.Num());
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
        TEXT("Run UObject::IsDataValid on assets currently selected in the Content Browser.\n"
             "Returns per-asset result (valid/invalid/not_validated) with error + warning text.\n"
             "Workflow: utility/focus_asset_in_browser first, then call this."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("validate_level"),
        TEXT("Validate the current editor world plus every on-disk dependency of its package.\n"
             "Capped at 256 dep assets to avoid overload. Engine/script deps excluded.\n"
             "Workflow: Use before checking in a level; follow with content_browser/save for any fixes."))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("list_errors"),
        TEXT("Return counts of messages in Map Check, Asset Check, AssetTools, and LoadErrors logs.\n"
             "No params. Useful as a quick health gauge before/after large operations."))
        .Build());

    return Tools;
}

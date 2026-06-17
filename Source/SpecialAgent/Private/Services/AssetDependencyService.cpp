#include "Services/AssetDependencyService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace
{
    FName PackageNameFromAssetPath(const FString& AssetOrPackagePath)
    {
        // Accept either /Game/Foo/Bar or /Game/Foo/Bar.Bar — both normalize to /Game/Foo/Bar.
        FString PackageName = AssetOrPackagePath;
        int32 DotIdx = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), DotIdx))
        {
            PackageName.LeftInline(DotIdx);
        }
        return FName(*PackageName);
    }

    IAssetRegistry& GetAssetRegistry()
    {
        FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        return Module.Get();
    }

    void AppendNames(TSharedPtr<FJsonObject>& Result, const TCHAR* Field, const TArray<FName>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Names.Num());
        for (const FName& N : Names)
        {
            Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
        }
        Result->SetArrayField(Field, Arr);
    }

    // Recursive helper for the dependency graph (bounded by MaxDepth).
    TSharedPtr<FJsonObject> BuildDepTree(IAssetRegistry& Registry, const FName& Package, int32 Depth, int32 MaxDepth, TSet<FName>& Visited)
    {
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("package"), Package.ToString());

        if (Depth >= MaxDepth)
        {
            Node->SetBoolField(TEXT("truncated"), true);
            return Node;
        }
        if (Visited.Contains(Package))
        {
            Node->SetBoolField(TEXT("cycle"), true);
            return Node;
        }
        Visited.Add(Package);

        TArray<FName> Deps;
        Registry.GetDependencies(Package, Deps, UE::AssetRegistry::EDependencyCategory::Package);

        TArray<TSharedPtr<FJsonValue>> ChildArr;
        for (const FName& Dep : Deps)
        {
            // Skip engine / script packages to keep graph manageable.
            const FString DepStr = Dep.ToString();
            if (DepStr.StartsWith(TEXT("/Script/")) || DepStr.StartsWith(TEXT("/Engine/")))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Child = BuildDepTree(Registry, Dep, Depth + 1, MaxDepth, Visited);
            ChildArr.Add(MakeShared<FJsonValueObject>(Child));
        }
        Node->SetArrayField(TEXT("dependencies"), ChildArr);
        return Node;
    }
}

FString FAssetDependencyService::GetServiceDescription() const
{
    return TEXT("Query asset references, referencers, and dependency graphs via Asset Registry");
}

FMCPResponse FAssetDependencyService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("get_references"))
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
            IAssetRegistry& Registry = GetAssetRegistry();
            const FName Package = PackageNameFromAssetPath(AssetPath);

            TArray<FName> Deps;
            const bool bOk = Registry.GetDependencies(Package, Deps, UE::AssetRegistry::EDependencyCategory::Package);

            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            Result->SetStringField(TEXT("package_name"), Package.ToString());
            AppendNames(Result, TEXT("references"), Deps);
            Result->SetNumberField(TEXT("count"), Deps.Num());
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: asset_deps/get_references '%s' → %d"), *Package.ToString(), Deps.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_referencers"))
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
            IAssetRegistry& Registry = GetAssetRegistry();
            const FName Package = PackageNameFromAssetPath(AssetPath);

            TArray<FName> Refs;
            const bool bOk = Registry.GetReferencers(Package, Refs, UE::AssetRegistry::EDependencyCategory::Package);

            Result->SetBoolField(TEXT("success"), bOk);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            Result->SetStringField(TEXT("package_name"), Package.ToString());
            AppendNames(Result, TEXT("referencers"), Refs);
            Result->SetNumberField(TEXT("count"), Refs.Num());
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: asset_deps/get_referencers '%s' → %d"), *Package.ToString(), Refs.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("find_unused"))
    {
        FString Root = TEXT("/Game");
        if (Request.Params.IsValid())
        {
            FMCPJson::ReadString(Request.Params, TEXT("root_path"), Root);
        }

        int32 MaxResults = 500;
        if (Request.Params.IsValid())
        {
            FMCPJson::ReadInteger(Request.Params, TEXT("max_results"), MaxResults);
        }

        auto Task = [Root, MaxResults]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            IAssetRegistry& Registry = GetAssetRegistry();

            FARFilter Filter;
            Filter.PackagePaths.Add(FName(*Root));
            Filter.bRecursivePaths = true;

            TArray<FAssetData> AllAssets;
            Registry.GetAssets(Filter, AllAssets);

            TArray<TSharedPtr<FJsonValue>> UnusedArr;
            int32 Scanned = 0;
            for (const FAssetData& Data : AllAssets)
            {
                ++Scanned;
                TArray<FName> Refs;
                Registry.GetReferencers(Data.PackageName, Refs, UE::AssetRegistry::EDependencyCategory::Package);
                if (Refs.Num() == 0)
                {
                    TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
                    AssetObj->SetStringField(TEXT("package_name"), Data.PackageName.ToString());
                    AssetObj->SetStringField(TEXT("asset_name"), Data.AssetName.ToString());
                    AssetObj->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
                    UnusedArr.Add(MakeShared<FJsonValueObject>(AssetObj));
                    if (UnusedArr.Num() >= MaxResults)
                    {
                        break;
                    }
                }
            }

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("root_path"), Root);
            Result->SetNumberField(TEXT("scanned"), Scanned);
            Result->SetNumberField(TEXT("unused_count"), UnusedArr.Num());
            Result->SetArrayField(TEXT("unused"), UnusedArr);
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: asset_deps/find_unused '%s' scanned=%d unused=%d"),
                *Root, Scanned, UnusedArr.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    if (MethodName == TEXT("get_dependency_graph"))
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

        int32 MaxDepth = 3;
        FMCPJson::ReadInteger(Request.Params, TEXT("max_depth"), MaxDepth);
        MaxDepth = FMath::Clamp(MaxDepth, 1, 5);

        auto Task = [AssetPath, MaxDepth]() -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            IAssetRegistry& Registry = GetAssetRegistry();
            const FName Package = PackageNameFromAssetPath(AssetPath);

            TSet<FName> Visited;
            TSharedPtr<FJsonObject> Tree = BuildDepTree(Registry, Package, /*Depth=*/0, MaxDepth, Visited);

            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("asset_path"), AssetPath);
            Result->SetNumberField(TEXT("max_depth"), MaxDepth);
            Result->SetObjectField(TEXT("graph"), Tree);
            Result->SetNumberField(TEXT("unique_packages"), Visited.Num());
            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: asset_deps/get_dependency_graph '%s' depth=%d unique=%d"),
                *Package.ToString(), MaxDepth, Visited.Num());
            return Result;
        };

        TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
        return FMCPResponse::Success(Request.Id, Result);
    }

    return MethodNotFound(Request.Id, TEXT("asset_deps"), MethodName);
}

TArray<FMCPToolInfo> FAssetDependencyService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(TEXT("get_references"),
        TEXT("List the packages this asset references (forward, package-category dependencies). Returns {success, asset_path, package_name, references:[package names], count}. "
             "Params: asset_path (string, required, /Game/... object path /Game/Foo.Foo or package path /Game/Foo - both normalize to the package). "
             "Workflow: use asset_deps/get_referencers for the reverse (who depends on me) direction. "
             "Warning: read-only, uses cached on-disk Asset Registry data only - unsaved in-memory edits are not reflected."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset object path or package path"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_referencers"),
        TEXT("List the packages that reference this asset (reverse, package-category dependencies). Returns {success, asset_path, package_name, referencers:[package names], count}. "
             "Params: asset_path (string, required, /Game/... object path or package path). "
             "Workflow: run before assets/delete or content_browser/delete to audit inbound uses; zero referencers makes it a find_unused candidate. "
             "Warning: read-only, uses cached on-disk Asset Registry data only; level/map references and recently-added in-memory assets may be missing."))
        .RequiredString(TEXT("asset_path"), TEXT("Asset object path or package path"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("find_unused"),
        TEXT("Enumerate assets under a content root that have zero referencers (deletion candidates). Returns {success, root_path, scanned, unused_count, unused:[{package_name, asset_name, class}]}. "
             "Params: root_path (string, optional, default /Game, scanned recursively), max_results (integer, optional, default 500 - stops scanning once reached). "
             "Workflow: verify each candidate (re-check asset_deps/get_referencers), then assets/delete or content_browser/delete. "
             "Warning: read-only here but the candidates are not safe to bulk-delete blindly - level/map assets, assets referenced only at runtime, and recently-added assets can show as unused. Scans every asset under root, so it is slow on large roots."))
        .OptionalString(TEXT("root_path"), TEXT("Root content path to scan recursively (default /Game)"))
        .OptionalInteger(TEXT("max_results"), TEXT("Maximum unused assets to return (default 500)"))
        .Build());

    Tools.Add(FMCPToolBuilder(TEXT("get_dependency_graph"),
        TEXT("Build a recursive package-dependency tree rooted at an asset. Returns {success, asset_path, max_depth, unique_packages, graph:{package, dependencies:[...], truncated?, cycle?}}. "
             "Params: asset_path (string, required, /Game/... object or package path), max_depth (integer, optional, default 3, clamped to 1-5). "
             "Workflow: start at depth 2 for materials/maps which fan out widely; /Script/ and /Engine/ dependencies are pruned and cycles are flagged with cycle:true. "
             "Warning: read-only, on-disk registry data only; deep graphs can be large - nodes at max_depth are marked truncated:true rather than expanded."))
        .RequiredString(TEXT("asset_path"), TEXT("Root asset object path or package path"))
        .OptionalInteger(TEXT("max_depth"), TEXT("Recursion depth, clamped to [1,5] (default 3)"))
        .Build());

    return Tools;
}

#include "Services/PCGService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

#include "PCGGraph.h"
#include "PCGComponent.h"
#include "PCGVolume.h"

FString FPCGService::GetServiceDescription() const
{
    return TEXT("Procedural Content Generation - discover, execute, and spawn PCG graphs");
}

FMCPResponse FPCGService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("list_graphs"))     return HandleListGraphs(Request);
    if (MethodName == TEXT("execute_graph"))   return HandleExecuteGraph(Request);
    if (MethodName == TEXT("spawn_pcg_actor")) return HandleSpawnPCGActor(Request);

    return MethodNotFound(Request.Id, TEXT("pcg"), MethodName);
}

FMCPResponse FPCGService::HandleListGraphs(const FMCPRequest& Request)
{
    FString PathFilter;
    if (Request.Params.IsValid())
    {
        FMCPJson::ReadString(Request.Params, TEXT("path"), PathFilter);
    }

    auto Task = [PathFilter]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UPCGGraph::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        if (!PathFilter.IsEmpty())
        {
            Filter.PackagePaths.Add(*PathFilter);
            Filter.bRecursivePaths = true;
        }

        TArray<FAssetData> FoundAssets;
        AssetRegistry.GetAssets(Filter, FoundAssets);

        TArray<TSharedPtr<FJsonValue>> GraphsArr;
        for (const FAssetData& AssetData : FoundAssets)
        {
            TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
            GraphObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
            GraphObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
            GraphObj->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
            GraphsArr.Add(MakeShared<FJsonValueObject>(GraphObj));
        }

        Result->SetBoolField(TEXT("success"), true);
        Result->SetArrayField(TEXT("graphs"), GraphsArr);
        Result->SetNumberField(TEXT("count"), GraphsArr.Num());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Listed %d PCG graphs"), GraphsArr.Num());
        return Result;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPCGService::HandleExecuteGraph(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString GraphPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("graph_path"), GraphPath))
        return InvalidParams(Request.Id, TEXT("Missing 'graph_path'"));

    FString ActorName;
    if (!FMCPJson::ReadString(Request.Params, TEXT("actor_name"), ActorName))
        return InvalidParams(Request.Id, TEXT("Missing 'actor_name'"));

    bool bForce = true;
    FMCPJson::ReadBool(Request.Params, TEXT("force"), bForce);

    auto Task = [GraphPath, ActorName, bForce]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: pcg/execute_graph no editor world"));
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        AActor* Actor = FMCPActorResolver::ByLabel(World, ActorName);
        if (!Actor)
        {
            UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: pcg/execute_graph actor not found: %s"), *ActorName);
            return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        }

        UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
        if (!Graph)
        {
            UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: pcg/execute_graph graph not found: %s"), *GraphPath);
            return FMCPJson::MakeError(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: execute PCG graph")));
        Actor->Modify();

        UPCGComponent* PCGComp = Actor->FindComponentByClass<UPCGComponent>();
        if (!PCGComp)
        {
            PCGComp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), NAME_None, RF_Transactional);
            if (!PCGComp)
            {
                return FMCPJson::MakeError(TEXT("Failed to create UPCGComponent"));
            }
            Actor->AddInstanceComponent(PCGComp);
            PCGComp->RegisterComponent();
        }

        PCGComp->SetGraph(Graph);
        PCGComp->GenerateLocal(bForce);
        Actor->MarkPackageDirty();

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Out->SetStringField(TEXT("graph_path"), GraphPath);
        Out->SetBoolField(TEXT("generating"), PCGComp->IsGenerating());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: pcg/execute_graph started on %s with %s"), *ActorName, *GraphPath);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FPCGService::HandleSpawnPCGActor(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid())
        return InvalidParams(Request.Id, TEXT("Missing params"));

    FString GraphPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("graph_path"), GraphPath))
        return InvalidParams(Request.Id, TEXT("Missing 'graph_path'"));

    FVector Location;
    if (!FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location))
        return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' (expected [X,Y,Z])"));

    FVector Scale(10.0, 10.0, 10.0);
    FMCPJson::ReadVec3(Request.Params, TEXT("scale"), Scale);

    auto Task = [GraphPath, Location, Scale]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
        if (!Graph)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn PCG actor")));

        FActorSpawnParameters SpawnParams;
        APCGVolume* Volume =
            World->SpawnActor<APCGVolume>(APCGVolume::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (!Volume)
        {
            return FMCPJson::MakeError(TEXT("Failed to spawn APCGVolume"));
        }

        Volume->SetActorScale3D(Scale);

        if (UPCGComponent* PCGComp = Volume->PCGComponent)
        {
            PCGComp->SetGraph(Graph);
            PCGComp->GenerateLocal(true);
        }

        Volume->MarkPackageDirty();

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        // Nest the actor under "actor" for consistency with every other spawn tool.
        TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
        FMCPJson::WriteActor(ActorObj, Volume);
        Out->SetObjectField(TEXT("actor"), ActorObj);
        Out->SetStringField(TEXT("graph_path"), GraphPath);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Spawned APCGVolume '%s' with graph %s"),
            *Volume->GetActorLabel(), *GraphPath);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FPCGService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("list_graphs"),
            TEXT("Discover PCG (Procedural Content Generation) graph assets via the asset registry. "
                 "Returns {success, count, graphs:[{name, path (object path Pkg.Asset), package (package name)}]}. "
                 "Params: path (string, optional virtual content folder like /Game/PCG; recursive; omit to search the whole project). "
                 "Workflow: call first to find a graph_path, then feed it to pcg/execute_graph or pcg/spawn_pcg_actor. "
                 "Warning: reads cached registry metadata (no asset load); a graph imported this session may be missing until the registry rescans."))
        .OptionalString(TEXT("path"), TEXT("Virtual content folder to restrict the search, e.g. /Game/PCG (recursive). Omit to search everything. Not an OS path."))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("execute_graph"),
            TEXT("Run a PCG graph on an existing scene actor: finds (or adds) a PCGComponent on the actor, assigns the graph, and calls GenerateLocal. "
                 "Returns {success, actor_name (label), graph_path, generating (bool, true while still producing points/spawning)}. "
                 "Params: graph_path (string, virtual object path like /Game/PCG/MyGraph.MyGraph; from pcg/list_graphs), "
                 "actor_name (string, target actor label, resolved by label), force (bool, default true; regenerate even if the component is clean). "
                 "Workflow: pcg/list_graphs to get graph_path; ensure actor_name exists; for a brand-new procedural volume use pcg/spawn_pcg_actor instead. "
                 "Warning: generation runs on the game thread and may spawn many instances; the PCGComponent is added as a non-transient instance component but the change persists only if you save the level."))
        .RequiredString(TEXT("graph_path"), TEXT("Virtual object path of the PCG graph, e.g. /Game/PCG/MyGraph.MyGraph (not an OS path)"))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the target actor that hosts (or will receive) the PCGComponent"))
        .OptionalBool  (TEXT("force"),      TEXT("Regenerate even if the PCG component is up-to-date (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("spawn_pcg_actor"),
            TEXT("Spawn a new APCGVolume at a world location, assign a PCG graph to its built-in PCGComponent, and generate immediately. "
                 "Returns {success, actor:{actor_label, path, class, location, rotation, scale, tags}, graph_path}. Use this for a fresh procedural volume; to drive an existing actor use pcg/execute_graph. "
                 "Params: graph_path (string, virtual object path like /Game/PCG/MyGraph.MyGraph), location (array [X,Y,Z], world-space cm, +X forward +Y right +Z up), "
                 "scale (array [X,Y,Z], unitless, default [10,10,10]; the volume's bounds = base extent * scale and define the generation region). "
                 "Workflow: pcg/list_graphs to get graph_path; enlarge scale to cover the area you want populated; the spawned volume auto-selects in the level. "
                 "Warning: spawns into the editor world and generates on the game thread (can produce many instances); the actor is not saved to disk until you save the level."))
        .RequiredString(TEXT("graph_path"), TEXT("Virtual object path of the PCG graph to attach, e.g. /Game/PCG/MyGraph.MyGraph"))
        .RequiredVec3  (TEXT("location"),   TEXT("World-space spawn location [X,Y,Z] in cm (+X forward, +Y right, +Z up)"))
        .OptionalVec3  (TEXT("scale"),      TEXT("Volume scale [X,Y,Z], unitless; controls the generation bounds. Default [10,10,10]"))
        .Build());

    return Tools;
}

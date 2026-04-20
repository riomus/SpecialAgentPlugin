#include "Services/PCGService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Editor.h"
#include "Engine/World.h"

#include "PCGGraph.h"
#include "PCGComponent.h"
#include "PCGVolume.h"

FString FPCGService::GetServiceDescription() const
{
    return TEXT("Procedural Content Generation - discover, execute, and spawn PCG graphs");
}

FMCPResponse FPCGService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        FMCPJson::WriteActor(Out, Volume);
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
            TEXT("List PCG graph assets. Queries the asset registry for UPCGGraph. "
                 "Params: path (string, optional content path like /Game/PCG, filter). "
                 "Workflow: call first to discover graphs before execute_graph / spawn_pcg_actor. "
                 "Warning: returns cached asset registry; rescan the project if a new graph was just imported."))
        .OptionalString(TEXT("path"), TEXT("Optional content path prefix (e.g. /Game/PCG) to restrict the search"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("execute_graph"),
            TEXT("Execute a PCG graph on an existing actor. Attaches UPCGComponent if missing, sets the graph, and triggers GenerateLocal. "
                 "Params: graph_path (string, /Game/... path), actor_name (string, actor label), force (bool, regenerate even if up-to-date). "
                 "Workflow: pair with pcg/list_graphs to discover graphs. "
                 "Warning: generation is async; poll the target actor to observe completion."))
        .RequiredString(TEXT("graph_path"), TEXT("Full asset path of the PCG graph, e.g. /Game/PCG/MyGraph.MyGraph"))
        .RequiredString(TEXT("actor_name"), TEXT("Label of the target actor hosting (or receiving) the UPCGComponent"))
        .OptionalBool  (TEXT("force"),      TEXT("Force regeneration even if the PCG component is clean (default true)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("spawn_pcg_actor"),
            TEXT("Spawn an APCGVolume at a location, assign a PCG graph, and generate. "
                 "Params: graph_path (string, /Game/... path), location ([X,Y,Z] cm world), scale ([X,Y,Z] default [10,10,10]). "
                 "Workflow: use pcg/list_graphs to discover the graph first. "
                 "Warning: the spawned volume is a full AVolume with brush geometry; adjust brush bounds after spawn if needed."))
        .RequiredString(TEXT("graph_path"), TEXT("Full asset path of the PCG graph to attach"))
        .RequiredVec3  (TEXT("location"),   TEXT("World-space spawn location [X,Y,Z] in cm"))
        .OptionalVec3  (TEXT("scale"),      TEXT("Volume scale [X,Y,Z]; default [10,10,10]"))
        .Build());

    return Tools;
}

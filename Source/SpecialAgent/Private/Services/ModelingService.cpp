#include "Services/ModelingService.h"

#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"

#include "UDynamicMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/GeometryScriptSelectionTypes.h"

namespace
{
    static UStaticMesh* GetStaticMeshAssetFromActor(AActor* Actor)
    {
        if (!Actor) return nullptr;
        if (AStaticMeshActor* SMActor = Cast<AStaticMeshActor>(Actor))
        {
            if (UStaticMeshComponent* Comp = SMActor->GetStaticMeshComponent())
            {
                return Comp->GetStaticMesh();
            }
        }
        if (UStaticMeshComponent* Comp = Actor->FindComponentByClass<UStaticMeshComponent>())
        {
            return Comp->GetStaticMesh();
        }
        return nullptr;
    }

    static UDynamicMesh* CopyStaticMeshToDynamic(UStaticMesh* Asset)
    {
        if (!Asset) return nullptr;

        UDynamicMesh* Dynamic = NewObject<UDynamicMesh>();
        FGeometryScriptCopyMeshFromAssetOptions ReadOptions;
        FGeometryScriptMeshReadLOD ReadLOD;
        ReadLOD.LODType  = EGeometryScriptLODType::MaxAvailable;
        ReadLOD.LODIndex = 0;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
            Asset, Dynamic, ReadOptions, ReadLOD, Outcome, /*bUseSectionMaterials=*/true, nullptr);

        if (Outcome != EGeometryScriptOutcomePins::Success)
        {
            return nullptr;
        }
        return Dynamic;
    }

    static bool WriteDynamicToStaticMesh(UDynamicMesh* Dynamic, UStaticMesh* Asset)
    {
        if (!Dynamic || !Asset) return false;

        FGeometryScriptCopyMeshToAssetOptions WriteOptions;
        WriteOptions.bEmitTransaction = true;
        FGeometryScriptMeshWriteLOD WriteLOD;
        WriteLOD.LODIndex = 0;
        EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

        UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
            Dynamic, Asset, WriteOptions, WriteLOD, Outcome, /*bUseSectionMaterials=*/true, nullptr);

        return Outcome == EGeometryScriptOutcomePins::Success;
    }

    static TSharedPtr<FJsonObject> ApplyBoolean(
        const FString& TargetName,
        const FString& ToolName,
        EGeometryScriptBooleanOperation Operation)
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return FMCPJson::MakeError(TEXT("No editor world"));
        }

        AActor* TargetActor = FMCPActorResolver::ByLabel(World, TargetName);
        AActor* ToolActor   = FMCPActorResolver::ByLabel(World, ToolName);
        if (!TargetActor) return FMCPJson::MakeError(FString::Printf(TEXT("Target actor not found: %s"), *TargetName));
        if (!ToolActor)   return FMCPJson::MakeError(FString::Printf(TEXT("Tool actor not found: %s"),   *ToolName));

        UStaticMesh* TargetAsset = GetStaticMeshAssetFromActor(TargetActor);
        UStaticMesh* ToolAsset   = GetStaticMeshAssetFromActor(ToolActor);
        if (!TargetAsset) return FMCPJson::MakeError(TEXT("Target actor has no StaticMesh"));
        if (!ToolAsset)   return FMCPJson::MakeError(TEXT("Tool actor has no StaticMesh"));

        UDynamicMesh* TargetMesh = CopyStaticMeshToDynamic(TargetAsset);
        UDynamicMesh* ToolMesh   = CopyStaticMeshToDynamic(ToolAsset);
        if (!TargetMesh) return FMCPJson::MakeError(TEXT("Failed to extract DynamicMesh from target"));
        if (!ToolMesh)   return FMCPJson::MakeError(TEXT("Failed to extract DynamicMesh from tool"));

        FGeometryScriptMeshBooleanOptions Options;
        Options.OutputTransformSpace = EGeometryScriptBooleanOutputSpace::TargetTransformSpace;

        UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
            TargetMesh,
            TargetActor->GetActorTransform(),
            ToolMesh,
            ToolActor->GetActorTransform(),
            Operation,
            Options,
            nullptr);

        if (!WriteDynamicToStaticMesh(TargetMesh, TargetAsset))
        {
            return FMCPJson::MakeError(TEXT("Failed to write result back to StaticMesh asset"));
        }

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("target"), TargetActor->GetActorLabel());
        Out->SetStringField(TEXT("tool"),   ToolActor->GetActorLabel());
        Out->SetStringField(TEXT("target_asset"), TargetAsset->GetPathName());

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling boolean op=%d on target=%s tool=%s"),
            (int32)Operation, *TargetName, *ToolName);
        return Out;
    }
}

FString FModelingService::GetServiceDescription() const
{
    return TEXT("Mesh modeling via Geometry Script - boolean, extrude, and simplify static mesh actors");
}

FMCPResponse FModelingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
    if (MethodName == TEXT("boolean_union"))    return HandleBooleanUnion(Request);
    if (MethodName == TEXT("boolean_subtract")) return HandleBooleanSubtract(Request);
    if (MethodName == TEXT("extrude"))          return HandleExtrude(Request);
    if (MethodName == TEXT("simplify"))         return HandleSimplify(Request);

    return MethodNotFound(Request.Id, TEXT("modeling"), MethodName);
}

FMCPResponse FModelingService::HandleBooleanUnion(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
    FString Target, Tool;
    if (!FMCPJson::ReadString(Request.Params, TEXT("target_actor"), Target))
        return InvalidParams(Request.Id, TEXT("Missing 'target_actor'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("tool_actor"), Tool))
        return InvalidParams(Request.Id, TEXT("Missing 'tool_actor'"));

    auto Task = [Target, Tool]() -> TSharedPtr<FJsonObject>
    {
        return ApplyBoolean(Target, Tool, EGeometryScriptBooleanOperation::Union);
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FModelingService::HandleBooleanSubtract(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));
    FString Target, Tool;
    if (!FMCPJson::ReadString(Request.Params, TEXT("target_actor"), Target))
        return InvalidParams(Request.Id, TEXT("Missing 'target_actor'"));
    if (!FMCPJson::ReadString(Request.Params, TEXT("tool_actor"), Tool))
        return InvalidParams(Request.Id, TEXT("Missing 'tool_actor'"));

    auto Task = [Target, Tool]() -> TSharedPtr<FJsonObject>
    {
        return ApplyBoolean(Target, Tool, EGeometryScriptBooleanOperation::Subtract);
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FModelingService::HandleExtrude(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

    FString Target;
    if (!FMCPJson::ReadString(Request.Params, TEXT("target_actor"), Target))
        return InvalidParams(Request.Id, TEXT("Missing 'target_actor'"));

    double Distance = 100.0;
    FMCPJson::ReadNumber(Request.Params, TEXT("distance"), Distance);

    FVector Direction(0, 0, 1);
    FMCPJson::ReadVec3(Request.Params, TEXT("direction"), Direction);

    auto Task = [Target, Distance, Direction]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, Target);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *Target));

        UStaticMesh* Asset = GetStaticMeshAssetFromActor(Actor);
        if (!Asset) return FMCPJson::MakeError(TEXT("Actor has no StaticMesh"));

        UDynamicMesh* Mesh = CopyStaticMeshToDynamic(Asset);
        if (!Mesh) return FMCPJson::MakeError(TEXT("Failed to extract DynamicMesh"));

        FGeometryScriptMeshLinearExtrudeOptions Options;
        Options.Distance      = static_cast<float>(Distance);
        Options.DirectionMode = EGeometryScriptLinearExtrudeDirection::FixedDirection;
        Options.Direction     = Direction.GetSafeNormal();
        Options.AreaMode      = EGeometryScriptPolyOperationArea::EntireSelection;

        // Empty selection applies to the entire mesh per GeometryScript semantics.
        FGeometryScriptMeshSelection EmptySelection;

        UGeometryScriptLibrary_MeshModelingFunctions::ApplyMeshLinearExtrudeFaces(
            Mesh, Options, EmptySelection, nullptr);

        if (!WriteDynamicToStaticMesh(Mesh, Asset))
        {
            return FMCPJson::MakeError(TEXT("Failed to write result back to StaticMesh asset"));
        }

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("target"), Actor->GetActorLabel());
        Out->SetNumberField(TEXT("distance"), Distance);
        FMCPJson::WriteVec3(Out, TEXT("direction"), Options.Direction);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/extrude on %s distance=%.2f"), *Target, Distance);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FModelingService::HandleSimplify(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

    FString Target;
    if (!FMCPJson::ReadString(Request.Params, TEXT("target_actor"), Target))
        return InvalidParams(Request.Id, TEXT("Missing 'target_actor'"));

    int32 TargetTriangleCount = 0;
    FMCPJson::ReadInteger(Request.Params, TEXT("target_triangle_count"), TargetTriangleCount);

    auto Task = [Target, TargetTriangleCount]() -> TSharedPtr<FJsonObject>
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

        AActor* Actor = FMCPActorResolver::ByLabel(World, Target);
        if (!Actor) return FMCPJson::MakeError(FString::Printf(TEXT("Actor not found: %s"), *Target));

        UStaticMesh* Asset = GetStaticMeshAssetFromActor(Actor);
        if (!Asset) return FMCPJson::MakeError(TEXT("Actor has no StaticMesh"));

        UDynamicMesh* Mesh = CopyStaticMeshToDynamic(Asset);
        if (!Mesh) return FMCPJson::MakeError(TEXT("Failed to extract DynamicMesh"));

        if (TargetTriangleCount > 0)
        {
            FGeometryScriptSimplifyMeshOptions Options;
            UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
                Mesh, TargetTriangleCount, Options, nullptr);
        }
        else
        {
            // Default: planar simplify removes redundant coplanar triangles without changing shape.
            FGeometryScriptPlanarSimplifyOptions PlanarOpts;
            UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToPlanar(
                Mesh, PlanarOpts, nullptr);
        }

        if (!WriteDynamicToStaticMesh(Mesh, Asset))
        {
            return FMCPJson::MakeError(TEXT("Failed to write result back to StaticMesh asset"));
        }

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("target"), Actor->GetActorLabel());
        Out->SetNumberField(TEXT("target_triangle_count"), TargetTriangleCount);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/simplify on %s targetTris=%d"), *Target, TargetTriangleCount);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FModelingService::GetAvailableTools() const
{
    TArray<FMCPToolInfo> Tools;

    Tools.Add(FMCPToolBuilder(
            TEXT("boolean_union"),
            TEXT("Boolean-union two static mesh actors via GeometryScript (UDynamicMesh), merging the tool mesh into the target. "
                 "Returns {success, target (label), tool (label), target_asset (object path of the overwritten StaticMesh)}. "
                 "Params: target_actor (string, label of the actor whose StaticMesh asset is modified), tool_actor (string, label of the actor whose mesh is added). "
                 "Workflow: both actors must be AStaticMeshActor or expose a UStaticMeshComponent; world transforms are respected so position them as they should combine. "
                 "Warning: overwrites the target actor's shared source StaticMesh asset in place (every instance of that mesh changes); irreversible, so duplicate the asset first if reuse matters."))
        .RequiredString(TEXT("target_actor"), TEXT("Label of the actor whose StaticMesh asset is overwritten with the merged result"))
        .RequiredString(TEXT("tool_actor"),   TEXT("Label of the actor whose mesh is merged in (its own asset is left unchanged)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("boolean_subtract"),
            TEXT("Boolean-subtract a tool mesh from a target mesh via GeometryScript (UDynamicMesh), carving the tool's volume out of the target. "
                 "Returns {success, target (label), tool (label), target_asset (object path of the overwritten StaticMesh)}. "
                 "Params: target_actor (string, label of the actor whose StaticMesh asset is carved), tool_actor (string, label of the actor whose volume is removed). "
                 "Workflow: both actors need a UStaticMeshComponent; world transforms are used, so overlap the tool actor with the target where you want material removed. "
                 "Warning: overwrites the target actor's shared source StaticMesh asset in place (affects every instance); irreversible, so duplicate the asset first if it is reused elsewhere."))
        .RequiredString(TEXT("target_actor"), TEXT("Label of the actor whose StaticMesh asset is overwritten with the carved result"))
        .RequiredString(TEXT("tool_actor"),   TEXT("Label of the actor whose overlapping volume is subtracted (its own asset is left unchanged)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("extrude"),
            TEXT("Linear-extrude every face of a static mesh actor a fixed distance along one direction via GeometryScript (UDynamicMesh). "
                 "Returns {success, target (label), distance (cm), direction (the normalized [X,Y,Z] used)}. "
                 "Params: target_actor (string, label of the static mesh actor), distance (number, cm, default 100), "
                 "direction (array [X,Y,Z], mesh-local axes, normalized internally, default [0,0,1] = +Z up). "
                 "Workflow: run modeling/simplify afterward to drop redundant coplanar triangles the extrude introduces. "
                 "Warning: extrudes the WHOLE mesh (selection is empty) and overwrites the target's shared source StaticMesh asset in place (affects every instance); for picking specific faces use the editor Modeling Mode."))
        .RequiredString(TEXT("target_actor"), TEXT("Label of the static mesh actor to extrude"))
        .OptionalNumber(TEXT("distance"),     TEXT("Extrude distance in cm (default 100)"))
        .OptionalVec3  (TEXT("direction"),    TEXT("Extrude direction [X,Y,Z] in mesh-local axes; normalized internally. Default [0,0,1] (+Z up)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("simplify"),
            TEXT("Reduce a static mesh actor's triangle count via GeometryScript (UDynamicMesh). "
                 "Returns {success, target (label), target_triangle_count (the requested target, 0 when planar mode ran)}. "
                 "Params: target_actor (string, label of the static mesh actor), "
                 "target_triangle_count (integer, optional; >0 simplifies down toward that triangle budget; omit or 0 runs planar simplify only, which merges coplanar triangles without altering the shape). "
                 "Workflow: call assets/get_info before and after to confirm the new triangle count. "
                 "Warning: overwrites the target's shared source StaticMesh asset in place (affects every instance) and is irreversible; duplicate the asset first if it is reused elsewhere."))
        .RequiredString (TEXT("target_actor"),          TEXT("Label of the static mesh actor to simplify"))
        .OptionalInteger(TEXT("target_triangle_count"), TEXT("Target triangle budget; >0 simplifies toward it. Omit or 0 for planar simplify only (coplanar merge, shape-preserving)."))
        .Build());

    return Tools;
}

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
#include "Engine/StaticMeshSocket.h"
#include "Components/StaticMeshComponent.h"

#include "StaticMeshEditorSubsystem.h"
#include "ScopedTransaction.h"
#include "Dom/JsonValue.h"

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
    if (MethodName == TEXT("boolean_union"))        return HandleBooleanUnion(Request);
    if (MethodName == TEXT("boolean_subtract"))     return HandleBooleanSubtract(Request);
    if (MethodName == TEXT("extrude"))              return HandleExtrude(Request);
    if (MethodName == TEXT("simplify"))             return HandleSimplify(Request);
    if (MethodName == TEXT("add_simple_collision")) return HandleAddSimpleCollision(Request);
    if (MethodName == TEXT("set_nanite"))           return HandleSetNanite(Request);
    if (MethodName == TEXT("generate_lods"))        return HandleGenerateLods(Request);
    if (MethodName == TEXT("manage_sockets"))       return HandleManageSockets(Request);

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

FMCPResponse FModelingService::HandleAddSimpleCollision(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

    FString AssetPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
        return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));

    FString Shape;
    if (!FMCPJson::ReadString(Request.Params, TEXT("shape"), Shape))
        return InvalidParams(Request.Id, TEXT("Missing 'shape'"));

    auto Task = [AssetPath, Shape]() -> TSharedPtr<FJsonObject>
    {
        EScriptCollisionShapeType ShapeType;
        if      (Shape == TEXT("box"))     ShapeType = EScriptCollisionShapeType::Box;
        else if (Shape == TEXT("sphere"))  ShapeType = EScriptCollisionShapeType::Sphere;
        else if (Shape == TEXT("capsule")) ShapeType = EScriptCollisionShapeType::Capsule;
        else if (Shape == TEXT("ndop10x")) ShapeType = EScriptCollisionShapeType::NDOP10_X;
        else if (Shape == TEXT("ndop10y")) ShapeType = EScriptCollisionShapeType::NDOP10_Y;
        else if (Shape == TEXT("ndop10z")) ShapeType = EScriptCollisionShapeType::NDOP10_Z;
        else if (Shape == TEXT("ndop18"))  ShapeType = EScriptCollisionShapeType::NDOP18;
        else if (Shape == TEXT("ndop26"))  ShapeType = EScriptCollisionShapeType::NDOP26;
        else return FMCPJson::MakeError(FString::Printf(TEXT("Unknown shape: %s"), *Shape));

        if (!GEditor) return FMCPJson::MakeError(TEXT("No GEditor"));

        UStaticMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
        if (!Subsystem) return FMCPJson::MakeError(TEXT("StaticMeshEditorSubsystem unavailable"));

        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!Mesh) return FMCPJson::MakeError(FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add simple collision")));
        Mesh->Modify();

        const int32 Index = Subsystem->AddSimpleCollisionsWithNotification(Mesh, ShapeType, /*bApplyChanges=*/true);
        if (Index < 0)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("AddSimpleCollisions failed for shape %s"), *Shape));
        }

        Mesh->MarkPackageDirty();

        const int32 CollisionCount = Subsystem->GetSimpleCollisionCount(Mesh);

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
        Out->SetStringField(TEXT("shape"), Shape);
        Out->SetNumberField(TEXT("simple_collision_count"), CollisionCount >= 0 ? CollisionCount : (Index + 1));

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/add_simple_collision %s shape=%s index=%d"),
            *AssetPath, *Shape, Index);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FModelingService::HandleSetNanite(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

    FString AssetPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
        return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));

    bool bEnabled = false;
    if (!FMCPJson::ReadBool(Request.Params, TEXT("enabled"), bEnabled))
        return InvalidParams(Request.Id, TEXT("Missing 'enabled'"));

    bool bHasPositionPrecision = false;
    int32 PositionPrecision = 0;
    if (FMCPJson::ReadInteger(Request.Params, TEXT("position_precision"), PositionPrecision))
        bHasPositionPrecision = true;

    bool bHasKeepPercent = false;
    double KeepPercentTriangles = 1.0;
    if (FMCPJson::ReadNumber(Request.Params, TEXT("keep_percent_triangles"), KeepPercentTriangles))
        bHasKeepPercent = true;

    auto Task = [AssetPath, bEnabled, bHasPositionPrecision, PositionPrecision, bHasKeepPercent, KeepPercentTriangles]() -> TSharedPtr<FJsonObject>
    {
        if (!GEditor) return FMCPJson::MakeError(TEXT("No GEditor"));

        UStaticMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
        if (!Subsystem) return FMCPJson::MakeError(TEXT("StaticMeshEditorSubsystem unavailable"));

        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!Mesh) return FMCPJson::MakeError(FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));

        FMeshNaniteSettings Settings = Subsystem->GetNaniteSettings(Mesh);

        bool bChanged = false;
        if (static_cast<bool>(Settings.bEnabled) != bEnabled)
        {
            Settings.bEnabled = bEnabled;
            bChanged = true;
        }
        if (bHasPositionPrecision && Settings.PositionPrecision != PositionPrecision)
        {
            Settings.PositionPrecision = PositionPrecision;
            bChanged = true;
        }
        if (bHasKeepPercent)
        {
            const float NewKeep = static_cast<float>(KeepPercentTriangles);
            if (Settings.KeepPercentTriangles != NewKeep)
            {
                Settings.KeepPercentTriangles = NewKeep;
                bChanged = true;
            }
        }

        if (!bChanged)
        {
            TSharedPtr<FJsonObject> Skipped = FMCPJson::MakeSuccess();
            Skipped->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
            Skipped->SetBoolField(TEXT("nanite_enabled"), static_cast<bool>(Settings.bEnabled));
            Skipped->SetBoolField(TEXT("skipped"), true);

            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/set_nanite %s skipped (no change, enabled=%d)"),
                *AssetPath, bEnabled ? 1 : 0);
            return Skipped;
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set nanite settings")));
        Mesh->Modify();

        Subsystem->SetNaniteSettings(Mesh, Settings, /*bApplyChanges=*/true);

        Mesh->MarkPackageDirty();

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
        Out->SetBoolField(TEXT("nanite_enabled"), bEnabled);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/set_nanite %s enabled=%d"), *AssetPath, bEnabled ? 1 : 0);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FModelingService::HandleGenerateLods(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

    FString AssetPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
        return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));

    // Parse 'reduction_percentages' (required) and optional 'screen_sizes' up-front so the
    // game-thread lambda stays self-contained and never touches the request JSON object.
    const TArray<TSharedPtr<FJsonValue>>* PercentArr = nullptr;
    if (!Request.Params->TryGetArrayField(TEXT("reduction_percentages"), PercentArr) || PercentArr->Num() == 0)
        return InvalidParams(Request.Id, TEXT("Missing or empty 'reduction_percentages' array"));

    TArray<float> Percentages;
    Percentages.Reserve(PercentArr->Num());
    for (const TSharedPtr<FJsonValue>& Elem : *PercentArr)
    {
        double Value = 0.0;
        if (!Elem.IsValid() || !Elem->TryGetNumber(Value))
            return InvalidParams(Request.Id, TEXT("'reduction_percentages' must contain only numbers 0..1"));
        Percentages.Add(static_cast<float>(Value));
    }

    TArray<float> ScreenSizes;
    bool bHasScreenSizes = false;
    const TArray<TSharedPtr<FJsonValue>>* ScreenArr = nullptr;
    if (Request.Params->TryGetArrayField(TEXT("screen_sizes"), ScreenArr) && ScreenArr->Num() > 0)
    {
        if (ScreenArr->Num() != PercentArr->Num())
            return InvalidParams(Request.Id, TEXT("'screen_sizes' length must match 'reduction_percentages'"));
        ScreenSizes.Reserve(ScreenArr->Num());
        for (const TSharedPtr<FJsonValue>& Elem : *ScreenArr)
        {
            double Value = 0.0;
            if (!Elem.IsValid() || !Elem->TryGetNumber(Value))
                return InvalidParams(Request.Id, TEXT("'screen_sizes' must contain only numbers 0..1"));
            ScreenSizes.Add(static_cast<float>(Value));
        }
        bHasScreenSizes = true;
    }

    auto Task = [AssetPath, Percentages, ScreenSizes, bHasScreenSizes]() -> TSharedPtr<FJsonObject>
    {
        if (!GEditor) return FMCPJson::MakeError(TEXT("No GEditor"));

        UStaticMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
        if (!Subsystem) return FMCPJson::MakeError(TEXT("StaticMeshEditorSubsystem unavailable"));

        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!Mesh) return FMCPJson::MakeError(FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));

        FStaticMeshReductionOptions Options;
        // When explicit screen sizes are provided, drive the LOD swap distances from them;
        // otherwise let the engine compute them automatically.
        Options.bAutoComputeLODScreenSize = !bHasScreenSizes;
        Options.ReductionSettings.Reserve(Percentages.Num());
        for (int32 Index = 0; Index < Percentages.Num(); ++Index)
        {
            FStaticMeshReductionSettings Setting;
            Setting.PercentTriangles = Percentages[Index];
            if (bHasScreenSizes)
            {
                Setting.ScreenSize = ScreenSizes[Index];
            }
            Options.ReductionSettings.Add(Setting);
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: generate LODs")));
        Mesh->Modify();

        const int32 LodCount = Subsystem->SetLodsWithNotification(Mesh, Options, /*bApplyChanges=*/true);
        if (LodCount < 0)
        {
            return FMCPJson::MakeError(FString::Printf(TEXT("SetLodsWithNotification failed for %s"), *AssetPath));
        }

        Mesh->MarkPackageDirty();

        TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
        Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
        Out->SetNumberField(TEXT("lod_count"), LodCount);

        UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/generate_lods %s levels=%d lod_count=%d"),
            *AssetPath, Percentages.Num(), LodCount);
        return Out;
    };

    TSharedPtr<FJsonObject> Result =
        FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
    return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FModelingService::HandleManageSockets(const FMCPRequest& Request)
{
    if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

    FString AssetPath;
    if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
        return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));

    FString Action;
    if (!FMCPJson::ReadString(Request.Params, TEXT("action"), Action))
        return InvalidParams(Request.Id, TEXT("Missing 'action'"));

    const bool bIsList = (Action == TEXT("list"));

    FString SocketName;
    if (!bIsList && !FMCPJson::ReadString(Request.Params, TEXT("socket_name"), SocketName))
        return InvalidParams(Request.Id, TEXT("'add' and 'remove' require 'socket_name'"));

    // Transform is only consumed by 'add'; read with defaults so omitted fields are safe.
    FVector Location(0, 0, 0);
    FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);
    FRotator Rotation(0, 0, 0);
    FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);
    FVector Scale(1, 1, 1);
    FMCPJson::ReadVec3(Request.Params, TEXT("scale"), Scale);

    auto Task = [AssetPath, Action, SocketName, Location, Rotation, Scale]() -> TSharedPtr<FJsonObject>
    {
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!Mesh) return FMCPJson::MakeError(FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));

        if (Action == TEXT("list"))
        {
            TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
            Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
            Out->SetStringField(TEXT("action"), Action);

            TArray<TSharedPtr<FJsonValue>> Arr;
            for (const UStaticMeshSocket* Socket : Mesh->Sockets)
            {
                if (!Socket) continue;
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Socket->SocketName.ToString());
                FMCPJson::WriteVec3(Entry, TEXT("location"), Socket->RelativeLocation);
                FMCPJson::WriteRotator(Entry, TEXT("rotation"), Socket->RelativeRotation);
                FMCPJson::WriteVec3(Entry, TEXT("scale"), Socket->RelativeScale);
                Arr.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Out->SetArrayField(TEXT("sockets"), Arr);
            Out->SetNumberField(TEXT("socket_count"), Arr.Num());

            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/manage_sockets list %s count=%d"),
                *AssetPath, Arr.Num());
            return Out;
        }

        if (Action == TEXT("add"))
        {
            const FName SocketFName(*SocketName);

            // Idempotency: if a socket with this name already exists, do not append a duplicate.
            if (Mesh->FindSocket(SocketFName) != nullptr)
            {
                TSharedPtr<FJsonObject> Skipped = FMCPJson::MakeSuccess();
                Skipped->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
                Skipped->SetStringField(TEXT("action"), Action);
                Skipped->SetStringField(TEXT("socket_name"), SocketName);
                Skipped->SetBoolField(TEXT("skipped"), true);

                UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/manage_sockets add %s socket=%s skipped (exists)"),
                    *AssetPath, *SocketName);
                return Skipped;
            }

            const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: add static mesh socket")));
            Mesh->Modify();

            UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh);
            Socket->SocketName       = SocketFName;
            Socket->RelativeLocation = Location;
            Socket->RelativeRotation = Rotation;
            Socket->RelativeScale    = Scale;
            Mesh->AddSocket(Socket);

            Mesh->MarkPackageDirty();

            TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
            Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
            Out->SetStringField(TEXT("action"), Action);
            Out->SetStringField(TEXT("socket_name"), SocketName);
            Out->SetNumberField(TEXT("socket_count"), Mesh->Sockets.Num());

            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/manage_sockets add %s socket=%s"),
                *AssetPath, *SocketName);
            return Out;
        }

        if (Action == TEXT("remove"))
        {
            UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
            if (!Socket)
            {
                return FMCPJson::MakeError(FString::Printf(TEXT("Socket not found: %s"), *SocketName));
            }

            const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: remove static mesh socket")));
            Mesh->Modify();

            Mesh->RemoveSocket(Socket);

            Mesh->MarkPackageDirty();

            TSharedPtr<FJsonObject> Out = FMCPJson::MakeSuccess();
            Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
            Out->SetStringField(TEXT("action"), Action);
            Out->SetStringField(TEXT("socket_name"), SocketName);
            Out->SetNumberField(TEXT("socket_count"), Mesh->Sockets.Num());

            UE_LOG(LogTemp, Log, TEXT("SpecialAgent: modeling/manage_sockets remove %s socket=%s"),
                *AssetPath, *SocketName);
            return Out;
        }

        return FMCPJson::MakeError(FString::Printf(TEXT("Unknown action: %s (expected add/remove/list)"), *Action));
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

    Tools.Add(FMCPToolBuilder(
            TEXT("add_simple_collision"),
            TEXT("Add a simple collision primitive to a StaticMesh asset via UStaticMeshEditorSubsystem::AddSimpleCollisionsWithNotification, operating on the /Game asset PATH (not an actor). "
                 "Returns {success, asset_path (object path), shape (the requested shape), simple_collision_count (number of simple collision primitives after the add)}. "
                 "Params: asset_path (string, required, /Game object path of the UStaticMesh), shape (enum, required, one of box/sphere/capsule/ndop10x/ndop10y/ndop10z/ndop18/ndop26 -> Box, Sphere, Capsule, or K-DOP hull). "
                 "Workflow: NDOP shapes give tighter convex hulls than box/sphere; call assets/get_info afterward to confirm the new collision count. "
                 "Warning: edits the shared source StaticMesh asset in place (affects every instance) and triggers a rebuild on the game thread; the existing simple collisions are kept and the new one is appended."))
        .RequiredString(TEXT("asset_path"), TEXT("/Game object path of the UStaticMesh asset to add simple collision to"))
        .RequiredEnum  (TEXT("shape"),
            { TEXT("box"), TEXT("sphere"), TEXT("capsule"), TEXT("ndop10x"), TEXT("ndop10y"), TEXT("ndop10z"), TEXT("ndop18"), TEXT("ndop26") },
            TEXT("Simple collision shape: box/sphere/capsule, or a K-DOP hull (ndop10x/y/z 10-sided, ndop18 18-sided, ndop26 26-sided)"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("set_nanite"),
            TEXT("Enable or disable Nanite on a StaticMesh asset via UStaticMeshEditorSubsystem::SetNaniteSettings, operating on the /Game asset PATH (not an actor). "
                 "Returns {success, asset_path (object path), nanite_enabled (bool)}; if nothing changed it returns {success, asset_path, nanite_enabled, skipped:true} without rebuilding. "
                 "Params: asset_path (string, required, /Game object path of the UStaticMesh), enabled (bool, required, whether Nanite data is generated), "
                 "position_precision (integer, optional, FMeshNaniteSettings.PositionPrecision; step is 2^-precision cm), keep_percent_triangles (number 0..1, optional, KeepPercentTriangles fallback target where 1.0 keeps all triangles). "
                 "Workflow: enable Nanite on dense static meshes to skip manual LODs; pass keep_percent_triangles below 1.0 to thin the source triangles. "
                 "Warning: edits the shared source StaticMesh asset in place (affects every instance) and triggers a Nanite rebuild on the game thread; the call is skipped (no rebuild) when enabled and the provided fields already match."))
        .RequiredString (TEXT("asset_path"),             TEXT("/Game object path of the UStaticMesh asset to configure Nanite on"))
        .RequiredBool   (TEXT("enabled"),                TEXT("Whether Nanite data is generated for this mesh (sets FMeshNaniteSettings.bEnabled)"))
        .OptionalInteger(TEXT("position_precision"),     TEXT("Nanite PositionPrecision; quantization step is 2^-precision cm. Omit to leave unchanged"))
        .OptionalNumber (TEXT("keep_percent_triangles"), TEXT("Fraction of source triangles to keep (0..1); 1.0 = no reduction. Maps to KeepPercentTriangles. Omit to leave unchanged"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("generate_lods"),
            TEXT("Build a chain of reduction LODs on a StaticMesh asset via UStaticMeshEditorSubsystem::SetLodsWithNotification, operating on the /Game asset PATH (not an actor). "
                 "Returns {success, asset_path (object path), lod_count (total LODs on the mesh after the build, including LOD0)}. "
                 "Params: asset_path (string, required, /Game object path of the UStaticMesh), reduction_percentages (array of numbers 0..1, required, one PercentTriangles per new LOD where 1.0 keeps all triangles and 0.0 keeps none; e.g. [0.5, 0.25, 0.1] makes three reduced LODs), "
                 "screen_sizes (array of numbers 0..1, optional, one ScreenSize per LOD, MUST be the same length as reduction_percentages; when supplied auto-compute screen size is turned off and these drive the LOD swap distances, otherwise the engine computes them). "
                 "Workflow: order percentages high to low so each LOD is coarser than the last; call modeling/set_nanite instead when you want automatic Nanite LODs rather than discrete reduction LODs. "
                 "Warning: edits the shared source StaticMesh asset in place (affects every instance) and triggers a mesh reduction + rebuild on the game thread, which can be slow on dense meshes; existing manually authored LODs are replaced by the generated chain."))
        .RequiredString(TEXT("asset_path"),            TEXT("/Game object path of the UStaticMesh asset to generate reduction LODs on"))
        .RequiredAny   (TEXT("reduction_percentages"), TEXT("Array of numbers 0..1, one PercentTriangles per new LOD (1.0 keeps all triangles, 0.0 keeps none). Order high to low, e.g. [0.5, 0.25, 0.1]"))
        .OptionalAny   (TEXT("screen_sizes"),          TEXT("Array of numbers 0..1, one ScreenSize per LOD; same length as reduction_percentages. Omit to auto-compute LOD swap distances"))
        .Build());

    Tools.Add(FMCPToolBuilder(
            TEXT("manage_sockets"),
            TEXT("Add, remove, or list named attachment sockets (UStaticMeshSocket) on a StaticMesh asset, operating on the /Game asset PATH (not an actor). "
                 "Returns for add/remove {success, asset_path, action, socket_name, socket_count}; for list {success, asset_path, action, sockets:[{name, location[X,Y,Z], rotation[Pitch,Yaw,Roll], scale[X,Y,Z]}], socket_count}. "
                 "Params: asset_path (string, required, /Game object path of the UStaticMesh), action (enum, required, one of add/remove/list), socket_name (string, required for add/remove and ignored for list), "
                 "location (array [X,Y,Z], optional for add, RelativeLocation, default [0,0,0]), rotation (array [Pitch,Yaw,Roll], optional for add, RelativeRotation, default [0,0,0]), scale (array [X,Y,Z], optional for add, RelativeScale, default [1,1,1]). "
                 "Workflow: call with action=list first to see existing socket names, then add/remove by name; sockets are looked up by FName so names should be unique. "
                 "Warning: add/remove edit the shared source StaticMesh asset in place (affects every instance) inside a transaction and mark the package dirty; add is skipped (no change) when a socket of the same name already exists, and remove errors when the named socket is missing."))
        .RequiredString(TEXT("asset_path"),  TEXT("/Game object path of the UStaticMesh asset whose sockets are managed"))
        .RequiredEnum  (TEXT("action"),
            { TEXT("add"), TEXT("remove"), TEXT("list") },
            TEXT("Socket operation: add (create a new named socket), remove (delete the named socket), or list (enumerate all sockets)"))
        .OptionalString(TEXT("socket_name"), TEXT("Name of the socket to add or remove (FName); required for add/remove, ignored for list"))
        .OptionalVec3  (TEXT("location"),    TEXT("RelativeLocation [X,Y,Z] for action=add (default [0,0,0])"))
        .OptionalVec3  (TEXT("rotation"),    TEXT("RelativeRotation [Pitch,Yaw,Roll] for action=add (default [0,0,0])"))
        .OptionalVec3  (TEXT("scale"),       TEXT("RelativeScale [X,Y,Z] for action=add (default [1,1,1])"))
        .Build());

    return Tools;
}

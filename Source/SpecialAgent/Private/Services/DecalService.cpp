// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/DecalService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Engine/World.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"

namespace
{
	static ADecalActor* FindDecalByLabel(UWorld* World, const FString& ActorLabel)
	{
		AActor* A = FMCPActorResolver::ByLabel(World, ActorLabel);
		return Cast<ADecalActor>(A);
	}

	static void WriteDecal(const TSharedPtr<FJsonObject>& Out, ADecalActor* D)
	{
		if (!D) return;
		FMCPJson::WriteActor(Out, D);
		if (UDecalComponent* Comp = D->GetDecal())
		{
			FMCPJson::WriteVec3(Out, TEXT("decal_size"), Comp->DecalSize);
			if (UMaterialInterface* Mat = Comp->GetDecalMaterial())
			{
				Out->SetStringField(TEXT("decal_material"), Mat->GetPathName());
			}
		}
	}
}

FString FDecalService::GetServiceDescription() const
{
	return TEXT("Decals - spawn ADecalActor, set decal material and projected size");
}

FMCPResponse FDecalService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("spawn")) return HandleSpawn(Request);
	if (MethodName == TEXT("set_material")) return HandleSetMaterial(Request);
	if (MethodName == TEXT("set_size")) return HandleSetSize(Request);

	return MethodNotFound(Request.Id, TEXT("decal"), MethodName);
}

FMCPResponse FDecalService::HandleSpawn(const FMCPRequest& Request)
{
	FVector Location(0, 0, 0);
	FRotator Rotation(0, 0, 0);
	FString ActorLabel, MaterialPath;

	if (Request.Params.IsValid())
	{
		FMCPJson::ReadVec3(Request.Params, TEXT("location"), Location);
		FMCPJson::ReadRotator(Request.Params, TEXT("rotation"), Rotation);
		FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel);
		FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath);
	}

	auto Task = [Location, Rotation, ActorLabel, MaterialPath]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: spawn decal")));

		FActorSpawnParameters SP;
		ADecalActor* Decal = World->SpawnActor<ADecalActor>(ADecalActor::StaticClass(), Location, Rotation, SP);
		if (!Decal) return FMCPJson::MakeError(TEXT("Failed to spawn ADecalActor"));

		if (!ActorLabel.IsEmpty())
		{
			Decal->SetActorLabel(ActorLabel);
		}
		Decal->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();

		if (!MaterialPath.IsEmpty())
		{
			UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
			if (!Mat)
			{
				Result->SetStringField(TEXT("warning"), FString::Printf(TEXT("Could not load material: %s"), *MaterialPath));
			}
			else if (UDecalComponent* Comp = Decal->GetDecal())
			{
				Comp->SetDecalMaterial(Mat);
			}
		}

		TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		WriteDecal(ActorJson, Decal);
		Result->SetObjectField(TEXT("actor"), ActorJson);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: decal/spawn -> %s"), *Decal->GetActorLabel());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FDecalService::HandleSetMaterial(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel, MaterialPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path'"));

	auto Task = [ActorLabel, MaterialPath]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		ADecalActor* D = FindDecalByLabel(World, ActorLabel);
		if (!D) return FMCPJson::MakeError(FString::Printf(TEXT("DecalActor not found: %s"), *ActorLabel));

		UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Mat) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load material: %s"), *MaterialPath));

		UDecalComponent* Comp = D->GetDecal();
		if (!Comp) return FMCPJson::MakeError(TEXT("DecalActor has no UDecalComponent"));
		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set decal material")));
		Comp->Modify();
		Comp->SetDecalMaterial(Mat);
		D->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		WriteDecal(ActorJson, D);
		Result->SetObjectField(TEXT("actor"), ActorJson);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: decal/set_material %s -> %s"), *ActorLabel, *Mat->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FDecalService::HandleSetSize(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString ActorLabel;
	FVector Size;
	if (!FMCPJson::ReadString(Request.Params, TEXT("actor_label"), ActorLabel))
		return InvalidParams(Request.Id, TEXT("Missing 'actor_label'"));
	if (!FMCPJson::ReadVec3(Request.Params, TEXT("size"), Size))
		return InvalidParams(Request.Id, TEXT("Missing 'size' (array [X,Y,Z] cm)"));

	auto Task = [ActorLabel, Size]() -> TSharedPtr<FJsonObject>
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPJson::MakeError(TEXT("No editor world"));
		ADecalActor* D = FindDecalByLabel(World, ActorLabel);
		if (!D) return FMCPJson::MakeError(FString::Printf(TEXT("DecalActor not found: %s"), *ActorLabel));
		UDecalComponent* Comp = D->GetDecal();
		if (!Comp) return FMCPJson::MakeError(TEXT("DecalActor has no UDecalComponent"));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set decal size")));
		Comp->Modify();
		Comp->DecalSize = Size;
		Comp->MarkRenderStateDirty();
		D->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		WriteDecal(ActorJson, D);
		Result->SetObjectField(TEXT("actor"), ActorJson);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: decal/set_size %s -> (%f,%f,%f)"),
			*ActorLabel, Size.X, Size.Y, Size.Z);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

TArray<FMCPToolInfo> FDecalService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(TEXT("spawn"),
		TEXT("Spawn an ADecalActor into the editor world. A decal projects along the actor's -X axis, so orient -X to point into the receiving surface. "
			 "Returns {actor:{actor_label, ..., decal_size:[X,Y,Z], decal_material?}}; use actor.actor_label for set_material / set_size. "
			 "Params: location (array [X,Y,Z] world-space cm, optional, default origin), rotation (array [Pitch,Yaw,Roll] degrees, optional, default zero), actor_label (string, optional), material_path (string, decal-domain UMaterialInterface object path, optional). "
			 "Workflow: trace a surface to get a hit point + normal (e.g. viewport/trace_from_screen) -> place location at the hit and derive rotation so -X faces the surface -> set_material / set_size. "
			 "Warning: rotation order is [Pitch,Yaw,Roll] (note the editor Details panel labels rotation X=Roll/Y=Pitch/Z=Yaw); default DecalSize is (128,256,256) cm half-extents - call set_size to resize; if material_path fails to load the actor still spawns and a warning field is returned; not saved to disk - save the level to persist."))
		.OptionalVec3(TEXT("location"), TEXT("World-space [X,Y,Z] in cm; default origin [0,0,0]"))
		.OptionalVec3(TEXT("rotation"), TEXT("[Pitch,Yaw,Roll] in degrees; default [0,0,0]. Decal projects along the actor's -X axis"))
		.OptionalString(TEXT("actor_label"), TEXT("Custom actor label used as the handle for set_material / set_size"))
		.OptionalString(TEXT("material_path"), TEXT("Decal-domain UMaterialInterface object path, e.g. /Game/Decals/M_Splat.M_Splat"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_material"),
		TEXT("Assign a decal-domain UMaterialInterface to an existing ADecalActor's UDecalComponent. "
			 "Returns {actor:{actor_label, ..., decal_size, decal_material}} reflecting the new material. "
			 "Params: actor_label (string, label of a spawned ADecalActor, required), material_path (string, decal-domain UMaterialInterface object path Pkg.Asset, required). "
			 "Workflow: spawn the decal first; use material/list_parameters on the same material_path to introspect its parameters. "
			 "Warning: actor not found by label, or material that fails to load, errors; a material whose Material Domain is not Deferred Decal will not render correctly; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned ADecalActor"))
		.RequiredString(TEXT("material_path"), TEXT("Decal-domain UMaterialInterface object path, e.g. /Game/Decals/M_Splat.M_Splat"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_size"),
		TEXT("Set DecalSize (the half-extents of the decal's projection box) on an existing ADecalActor's UDecalComponent. "
			 "Returns {actor:{actor_label, ..., decal_size, decal_material}} reflecting the new size. "
			 "Params: actor_label (string, required), size (array [X,Y,Z] half-extents in cm, required): X = projection depth along the actor's -X axis, Y/Z = perpendicular half-width/half-height of the projected quad. "
			 "Workflow: spawn -> set_size to fit the surface; default after spawn is (128,256,256) cm. "
			 "Warning: actor not found by label errors; oversized projection boxes (large X depth especially) hurt render performance - keep them proportionate; only marks the package dirty, save the level to persist."))
		.RequiredString(TEXT("actor_label"), TEXT("Label of a spawned ADecalActor"))
		.RequiredVec3(TEXT("size"), TEXT("[X,Y,Z] half-extents in cm; X is projection depth along -X, Y/Z are perpendicular extents"))
		.Build());

	return Tools;
}

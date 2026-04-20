// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/DecalService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPActorResolver.h"
#include "MCPCommon/MCPToolBuilder.h"

#include "Engine/World.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "Editor.h"

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

FMCPResponse FDecalService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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

		FActorSpawnParameters SP;
		ADecalActor* Decal = World->SpawnActor<ADecalActor>(ADecalActor::StaticClass(), Location, Rotation, SP);
		if (!Decal) return FMCPJson::MakeError(TEXT("Failed to spawn ADecalActor"));

		if (!ActorLabel.IsEmpty())
		{
			Decal->SetActorLabel(ActorLabel);
		}

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
		TEXT("Spawn ADecalActor. Orient so the -X axis of the actor points into the surface.\n"
			 "Params: location ([X,Y,Z] cm), rotation ([Pitch,Yaw,Roll] deg optional), "
			 "actor_label (string optional), material_path (string optional).\n"
			 "Workflow: viewport/trace_from_screen returns a surface normal; compute rotation from it.\n"
			 "Warning: default DecalSize is (128,256,256) - call set_size to resize."))
		.OptionalVec3(TEXT("location"), TEXT("World-space [X,Y,Z] cm"))
		.OptionalVec3(TEXT("rotation"), TEXT("Optional [Pitch,Yaw,Roll] deg"))
		.OptionalString(TEXT("actor_label"), TEXT("Optional custom actor label"))
		.OptionalString(TEXT("material_path"), TEXT("Optional decal material path (UMaterialInterface)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_material"),
		TEXT("Assign a UMaterialInterface (Decal domain) to an ADecalActor.\n"
			 "Params: actor_label (string), material_path (string, UMaterialInterface path).\n"
			 "Workflow: use material/list_parameters to introspect decal material parameters.\n"
			 "Warning: non-decal-domain materials fail to render correctly."))
		.RequiredString(TEXT("actor_label"), TEXT("DecalActor label"))
		.RequiredString(TEXT("material_path"), TEXT("UMaterialInterface object path (e.g. /Game/Decals/M_Splat.M_Splat)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_size"),
		TEXT("Set DecalSize (projected volume extents) on an ADecalActor.\n"
			 "Params: actor_label (string), size ([X,Y,Z] cm).\n"
			 "Workflow: X is projection depth along -X; Y/Z are extents perpendicular.\n"
			 "Warning: very large extents hurt render performance; keep size proportionate."))
		.RequiredString(TEXT("actor_label"), TEXT("DecalActor label"))
		.RequiredVec3(TEXT("size"), TEXT("[X,Y,Z] cm - X is projection depth"))
		.Build());

	return Tools;
}

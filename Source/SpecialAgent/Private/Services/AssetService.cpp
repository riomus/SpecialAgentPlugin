// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/AssetService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"

FAssetService::FAssetService()
{
}

FString FAssetService::GetServiceDescription() const
{
	return TEXT("Asset discovery and management - browse Content Browser assets");
}

FMCPResponse FAssetService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("list"))
	{
		return HandleListAssets(Request);
	}
	else if (MethodName == TEXT("find"))
	{
		return HandleFindAsset(Request);
	}
	else if (MethodName == TEXT("get_properties"))
	{
		return HandleGetAssetProperties(Request);
	}
	else if (MethodName == TEXT("search"))
	{
		return HandleSearchAssets(Request);
	}
	else if (MethodName == TEXT("get_bounds"))
	{
		return HandleGetAssetBounds(Request);
	}
	else if (MethodName == TEXT("get_info"))
	{
		return HandleGetAssetInfo(Request);
	}
	else if (MethodName == TEXT("sync_to_browser")) return HandleSyncToBrowser(Request);
	else if (MethodName == TEXT("create_folder"))   return HandleCreateFolder(Request);
	else if (MethodName == TEXT("rename"))          return HandleRenameAsset(Request);
	else if (MethodName == TEXT("delete"))          return HandleDeleteAsset(Request);
	else if (MethodName == TEXT("move"))            return HandleMoveAsset(Request);
	else if (MethodName == TEXT("duplicate"))       return HandleDuplicateAsset(Request);
	else if (MethodName == TEXT("save"))            return HandleSaveAsset(Request);
	else if (MethodName == TEXT("set_metadata"))    return HandleSetMetadata(Request);
	else if (MethodName == TEXT("get_metadata"))    return HandleGetMetadata(Request);
	else if (MethodName == TEXT("validate"))        return HandleValidateAsset(Request);

	return MethodNotFound(Request.Id, TEXT("assets"), MethodName);
}

FMCPResponse FAssetService::HandleListAssets(const FMCPRequest& Request)
{
	// Get filter parameters
	FString ClassFilter;
	FString PathFilter;
	int32 MaxResults = 1000;

	if (Request.Params.IsValid())
	{
		const TSharedPtr<FJsonObject>* FilterObj;
		if (Request.Params->TryGetObjectField(TEXT("filter"), FilterObj))
		{
			(*FilterObj)->TryGetStringField(TEXT("class"), ClassFilter);
			(*FilterObj)->TryGetStringField(TEXT("path"), PathFilter);
			(*FilterObj)->TryGetNumberField(TEXT("max_results"), MaxResults);
		}
	}

	auto ListTask = [ClassFilter, PathFilter, MaxResults]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		// Build filter
		FARFilter Filter;
		if (!ClassFilter.IsEmpty())
		{
			Filter.ClassPaths.Add(FTopLevelAssetPath(ClassFilter));
		}
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
			Filter.bRecursivePaths = true;
		}

		// Get assets
		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAssets(Filter, AssetDataList);

		// Limit results
		if (AssetDataList.Num() > MaxResults)
		{
			AssetDataList.SetNum(MaxResults);
		}

		// Convert to JSON array
		TArray<TSharedPtr<FJsonValue>> AssetsJson;
		for (const FAssetData& AssetData : AssetDataList)
		{
			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
			AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
			AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
			AssetObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
			
			AssetsJson.Add(MakeShared<FJsonValueObject>(AssetObj));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("assets"), AssetsJson);
		Result->SetNumberField(TEXT("count"), AssetsJson.Num());
		Result->SetNumberField(TEXT("total_found"), AssetDataList.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Listed %d assets"), AssetsJson.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(ListTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleFindAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString Name;
	if (!Request.Params->TryGetStringField(TEXT("name"), Name))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'name'"));
	}

	auto FindTask = [Name]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		// Search for assets matching the name
		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAllAssets(AssetDataList);

		TArray<TSharedPtr<FJsonValue>> MatchingAssets;
		for (const FAssetData& AssetData : AssetDataList)
		{
			if (AssetData.AssetName.ToString().Contains(Name))
			{
				TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
				AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
				AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
				
				MatchingAssets.Add(MakeShared<FJsonValueObject>(AssetObj));

				if (MatchingAssets.Num() >= 100) break; // Limit results
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("assets"), MatchingAssets);
		Result->SetNumberField(TEXT("count"), MatchingAssets.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(FindTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetAssetProperties(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	auto GetPropertiesTask = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		
		if (!AssetData.IsValid())
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return Result;
		}

		// Build properties object
		TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
		PropertiesObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		PropertiesObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		PropertiesObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
		PropertiesObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		PropertiesObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());

		// Get tags
		TArray<TSharedPtr<FJsonValue>> TagsArray;
		for (const auto& TagPair : AssetData.TagsAndValues)
		{
			TSharedPtr<FJsonObject> TagObj = MakeShared<FJsonObject>();
			TagObj->SetStringField(TEXT("key"), TagPair.Key.ToString());
			TagObj->SetStringField(TEXT("value"), TagPair.Value.AsString());
			TagsArray.Add(MakeShared<FJsonValueObject>(TagObj));
		}
		PropertiesObj->SetArrayField(TEXT("tags"), TagsArray);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("properties"), PropertiesObj);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetPropertiesTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleSearchAssets(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString Query;
	if (!Request.Params->TryGetStringField(TEXT("query"), Query))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'query'"));
	}

	int32 MaxResults = 100;
	Request.Params->TryGetNumberField(TEXT("max_results"), MaxResults);

	auto SearchTask = [Query, MaxResults]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAllAssets(AssetDataList);

		// Search in name, path, and class
		FString QueryLower = Query.ToLower();
		TArray<TSharedPtr<FJsonValue>> MatchingAssets;
		
		for (const FAssetData& AssetData : AssetDataList)
		{
			bool bMatches = false;
			
			if (AssetData.AssetName.ToString().ToLower().Contains(QueryLower))
			{
				bMatches = true;
			}
			else if (AssetData.GetObjectPathString().ToLower().Contains(QueryLower))
			{
				bMatches = true;
			}
			else if (AssetData.AssetClassPath.ToString().ToLower().Contains(QueryLower))
			{
				bMatches = true;
			}

			if (bMatches)
			{
				TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
				AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
				AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
				
				MatchingAssets.Add(MakeShared<FJsonValueObject>(AssetObj));

				if (MatchingAssets.Num() >= MaxResults) break;
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("assets"), MatchingAssets);
		Result->SetNumberField(TEXT("count"), MatchingAssets.Num());
		Result->SetStringField(TEXT("query"), Query);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Search for '%s' found %d assets"), *Query, MatchingAssets.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SearchTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetAssetBounds(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	auto GetBoundsTask = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Try to load the asset
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found or failed to load: %s"), *AssetPath));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

		// Check if it's a StaticMesh
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (StaticMesh)
		{
			FBox BoundingBox = StaticMesh->GetBoundingBox();
			
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			
			// Min point
			TArray<TSharedPtr<FJsonValue>> MinArr;
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
			BoundsObj->SetArrayField(TEXT("min"), MinArr);
			
			// Max point
			TArray<TSharedPtr<FJsonValue>> MaxArr;
			MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.X));
			MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Y));
			MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Z));
			BoundsObj->SetArrayField(TEXT("max"), MaxArr);
			
			// Center point
			FVector Center = BoundingBox.GetCenter();
			TArray<TSharedPtr<FJsonValue>> CenterArr;
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
			BoundsObj->SetArrayField(TEXT("center"), CenterArr);
			
			// Extent (half-size)
			FVector Extent = BoundingBox.GetExtent();
			TArray<TSharedPtr<FJsonValue>> ExtentArr;
			ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.X));
			ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Y));
			ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Z));
			BoundsObj->SetArrayField(TEXT("extent"), ExtentArr);
			
			// Size (full dimensions)
			FVector Size = BoundingBox.GetSize();
			TArray<TSharedPtr<FJsonValue>> SizeArr;
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
			BoundsObj->SetArrayField(TEXT("size"), SizeArr);
			
			// Pivot offset - how far the origin (0,0,0) is from the mesh center
			// Negative values mean the pivot is below/behind center
			// Use -Center to get offset needed to center the mesh at spawn point
			TArray<TSharedPtr<FJsonValue>> PivotOffsetArr;
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.X));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Y));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Z));
			BoundsObj->SetArrayField(TEXT("pivot_offset"), PivotOffsetArr);
			
			// Bottom offset - how far below origin the mesh extends (add to Z to place on ground)
			BoundsObj->SetNumberField(TEXT("bottom_z"), -BoundingBox.Min.Z);
			
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			Result->SetStringField(TEXT("mesh_type"), TEXT("StaticMesh"));
			
			// Additional mesh info
			Result->SetNumberField(TEXT("num_lods"), StaticMesh->GetNumLODs());
			if (StaticMesh->GetRenderData() && StaticMesh->GetRenderData()->LODResources.Num() > 0)
			{
				Result->SetNumberField(TEXT("num_vertices"), StaticMesh->GetRenderData()->LODResources[0].GetNumVertices());
				Result->SetNumberField(TEXT("num_triangles"), StaticMesh->GetRenderData()->LODResources[0].GetNumTriangles());
			}
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got bounds for StaticMesh: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a SkeletalMesh
		USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
		if (SkeletalMesh)
		{
			FBox BoundingBox = SkeletalMesh->GetBounds().GetBox();
			
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			
			// Min point
			TArray<TSharedPtr<FJsonValue>> MinArr;
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
			BoundsObj->SetArrayField(TEXT("min"), MinArr);
			
			// Max point
			TArray<TSharedPtr<FJsonValue>> MaxArr;
			MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.X));
			MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Y));
			MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Z));
			BoundsObj->SetArrayField(TEXT("max"), MaxArr);
			
			// Center point
			FVector Center = BoundingBox.GetCenter();
			TArray<TSharedPtr<FJsonValue>> CenterArr;
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
			BoundsObj->SetArrayField(TEXT("center"), CenterArr);
			
			// Extent (half-size)
			FVector Extent = BoundingBox.GetExtent();
			TArray<TSharedPtr<FJsonValue>> ExtentArr;
			ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.X));
			ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Y));
			ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Z));
			BoundsObj->SetArrayField(TEXT("extent"), ExtentArr);
			
			// Size (full dimensions)
			FVector Size = BoundingBox.GetSize();
			TArray<TSharedPtr<FJsonValue>> SizeArr;
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
			BoundsObj->SetArrayField(TEXT("size"), SizeArr);
			
			// Pivot offset and bottom Z
			TArray<TSharedPtr<FJsonValue>> PivotOffsetArr;
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.X));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Y));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Z));
			BoundsObj->SetArrayField(TEXT("pivot_offset"), PivotOffsetArr);
			BoundsObj->SetNumberField(TEXT("bottom_z"), -BoundingBox.Min.Z);
			
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			Result->SetStringField(TEXT("mesh_type"), TEXT("SkeletalMesh"));
			
			// Additional skeletal mesh info
			Result->SetNumberField(TEXT("num_bones"), SkeletalMesh->GetRefSkeleton().GetNum());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got bounds for SkeletalMesh: %s"), *AssetPath);
			return Result;
		}

		// Asset is not a mesh type we can get bounds from
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset is not a mesh type (StaticMesh or SkeletalMesh): %s"), *Asset->GetClass()->GetName()));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetBoundsTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetAssetInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	auto GetInfoTask = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Try to load the asset
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found or failed to load: %s"), *AssetPath));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_name"), Asset->GetName());
		Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
		Result->SetStringField(TEXT("outer_path"), Asset->GetOuter() ? Asset->GetOuter()->GetPathName() : TEXT("None"));

		// Check if it's a StaticMesh - provide detailed info
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (StaticMesh)
		{
			Result->SetStringField(TEXT("type"), TEXT("StaticMesh"));
			
			// Bounds info
			FBox BoundingBox = StaticMesh->GetBoundingBox();
			FVector Size = BoundingBox.GetSize();
			FVector Center = BoundingBox.GetCenter();
			
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> SizeArr, CenterArr, MinArr;
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
			
			BoundsObj->SetArrayField(TEXT("size"), SizeArr);
			BoundsObj->SetArrayField(TEXT("center"), CenterArr);
			BoundsObj->SetArrayField(TEXT("min"), MinArr);
			BoundsObj->SetNumberField(TEXT("bottom_z_offset"), -BoundingBox.Min.Z);
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			
			// Mesh stats
			Result->SetNumberField(TEXT("num_lods"), StaticMesh->GetNumLODs());
			if (StaticMesh->GetRenderData() && StaticMesh->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD0 = StaticMesh->GetRenderData()->LODResources[0];
				Result->SetNumberField(TEXT("num_vertices"), LOD0.GetNumVertices());
				Result->SetNumberField(TEXT("num_triangles"), LOD0.GetNumTriangles());
				Result->SetNumberField(TEXT("num_sections"), LOD0.Sections.Num());
			}
			
			// Materials
			TArray<TSharedPtr<FJsonValue>> MaterialsArr;
			for (int32 i = 0; i < StaticMesh->GetStaticMaterials().Num(); i++)
			{
				const FStaticMaterial& MatSlot = StaticMesh->GetStaticMaterials()[i];
				TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
				MatObj->SetNumberField(TEXT("index"), i);
				MatObj->SetStringField(TEXT("slot_name"), MatSlot.MaterialSlotName.ToString());
				if (MatSlot.MaterialInterface)
				{
					MatObj->SetStringField(TEXT("material_name"), MatSlot.MaterialInterface->GetName());
					MatObj->SetStringField(TEXT("material_path"), MatSlot.MaterialInterface->GetPathName());
				}
				else
				{
					MatObj->SetStringField(TEXT("material_name"), TEXT("None"));
				}
				MaterialsArr.Add(MakeShared<FJsonValueObject>(MatObj));
			}
			Result->SetArrayField(TEXT("materials"), MaterialsArr);
			Result->SetNumberField(TEXT("num_materials"), MaterialsArr.Num());
			
			// Collision info
			if (StaticMesh->GetBodySetup())
			{
				UBodySetup* BodySetup = StaticMesh->GetBodySetup();
				TSharedPtr<FJsonObject> CollisionObj = MakeShared<FJsonObject>();
				CollisionObj->SetBoolField(TEXT("has_collision"), true);
				CollisionObj->SetStringField(TEXT("collision_complexity"), 
					BodySetup->CollisionTraceFlag == CTF_UseDefault ? TEXT("Default") :
					BodySetup->CollisionTraceFlag == CTF_UseSimpleAndComplex ? TEXT("SimpleAndComplex") :
					BodySetup->CollisionTraceFlag == CTF_UseSimpleAsComplex ? TEXT("SimpleAsComplex") :
					BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple ? TEXT("ComplexAsSimple") : TEXT("Unknown")
				);
				CollisionObj->SetNumberField(TEXT("num_convex_elements"), BodySetup->AggGeom.ConvexElems.Num());
				CollisionObj->SetNumberField(TEXT("num_box_elements"), BodySetup->AggGeom.BoxElems.Num());
				CollisionObj->SetNumberField(TEXT("num_sphere_elements"), BodySetup->AggGeom.SphereElems.Num());
				CollisionObj->SetNumberField(TEXT("num_capsule_elements"), BodySetup->AggGeom.SphylElems.Num());
				Result->SetObjectField(TEXT("collision"), CollisionObj);
			}
			else
			{
				TSharedPtr<FJsonObject> CollisionObj = MakeShared<FJsonObject>();
				CollisionObj->SetBoolField(TEXT("has_collision"), false);
				Result->SetObjectField(TEXT("collision"), CollisionObj);
			}
			
			// Nanite info
			Result->SetBoolField(TEXT("nanite_enabled"), StaticMesh->IsNaniteEnabled());
			
			// Lightmap info
			Result->SetNumberField(TEXT("lightmap_resolution"), StaticMesh->GetLightMapResolution());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for StaticMesh: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a SkeletalMesh
		USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
		if (SkeletalMesh)
		{
			Result->SetStringField(TEXT("type"), TEXT("SkeletalMesh"));
			
			// Bounds info
			FBox BoundingBox = SkeletalMesh->GetBounds().GetBox();
			FVector Size = BoundingBox.GetSize();
			FVector Center = BoundingBox.GetCenter();
			
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> SizeArr, CenterArr;
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
			
			BoundsObj->SetArrayField(TEXT("size"), SizeArr);
			BoundsObj->SetArrayField(TEXT("center"), CenterArr);
			BoundsObj->SetNumberField(TEXT("bottom_z_offset"), -BoundingBox.Min.Z);
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			
			// Skeleton info
			Result->SetNumberField(TEXT("num_bones"), SkeletalMesh->GetRefSkeleton().GetNum());
			
			// Materials
			TArray<TSharedPtr<FJsonValue>> MaterialsArr;
			for (int32 i = 0; i < SkeletalMesh->GetMaterials().Num(); i++)
			{
				const FSkeletalMaterial& MatSlot = SkeletalMesh->GetMaterials()[i];
				TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
				MatObj->SetNumberField(TEXT("index"), i);
				MatObj->SetStringField(TEXT("slot_name"), MatSlot.MaterialSlotName.ToString());
				if (MatSlot.MaterialInterface)
				{
					MatObj->SetStringField(TEXT("material_name"), MatSlot.MaterialInterface->GetName());
				}
				else
				{
					MatObj->SetStringField(TEXT("material_name"), TEXT("None"));
				}
				MaterialsArr.Add(MakeShared<FJsonValueObject>(MatObj));
			}
			Result->SetArrayField(TEXT("materials"), MaterialsArr);
			Result->SetNumberField(TEXT("num_materials"), MaterialsArr.Num());
			
			// LOD info
			Result->SetNumberField(TEXT("num_lods"), SkeletalMesh->GetLODNum());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for SkeletalMesh: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a Material
		UMaterialInterface* Material = Cast<UMaterialInterface>(Asset);
		if (Material)
		{
			Result->SetStringField(TEXT("type"), TEXT("Material"));
			Result->SetStringField(TEXT("material_domain"), 
				Material->GetMaterial() ? 
					(Material->GetMaterial()->MaterialDomain == MD_Surface ? TEXT("Surface") :
					 Material->GetMaterial()->MaterialDomain == MD_DeferredDecal ? TEXT("DeferredDecal") :
					 Material->GetMaterial()->MaterialDomain == MD_LightFunction ? TEXT("LightFunction") :
					 Material->GetMaterial()->MaterialDomain == MD_PostProcess ? TEXT("PostProcess") :
					 Material->GetMaterial()->MaterialDomain == MD_UI ? TEXT("UI") : TEXT("Unknown"))
				: TEXT("Unknown")
			);
			Result->SetBoolField(TEXT("is_two_sided"), Material->IsTwoSided());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for Material: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a Blueprint
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
		if (Blueprint)
		{
			Result->SetStringField(TEXT("type"), TEXT("Blueprint"));
			Result->SetStringField(TEXT("blueprint_type"), 
				Blueprint->BlueprintType == BPTYPE_Normal ? TEXT("Normal") :
				Blueprint->BlueprintType == BPTYPE_Const ? TEXT("Const") :
				Blueprint->BlueprintType == BPTYPE_MacroLibrary ? TEXT("MacroLibrary") :
				Blueprint->BlueprintType == BPTYPE_Interface ? TEXT("Interface") :
				Blueprint->BlueprintType == BPTYPE_LevelScript ? TEXT("LevelScript") :
				Blueprint->BlueprintType == BPTYPE_FunctionLibrary ? TEXT("FunctionLibrary") : TEXT("Unknown")
			);
			if (Blueprint->ParentClass)
			{
				Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
			}
			if (Blueprint->GeneratedClass)
			{
				Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass->GetName());
			}
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for Blueprint: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a Texture
		UTexture* Texture = Cast<UTexture>(Asset);
		if (Texture)
		{
			Result->SetStringField(TEXT("type"), TEXT("Texture"));
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D)
			{
				Result->SetNumberField(TEXT("width"), Texture2D->GetSizeX());
				Result->SetNumberField(TEXT("height"), Texture2D->GetSizeY());
				Result->SetNumberField(TEXT("num_mips"), Texture2D->GetNumMips());
			}
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for Texture: %s"), *AssetPath);
			return Result;
		}

		// Generic asset - just basic info
		Result->SetStringField(TEXT("type"), TEXT("Other"));
		Result->SetStringField(TEXT("description"), TEXT("Asset loaded but type-specific info not available. Use get_properties for raw property data."));
		
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got basic info for asset: %s (%s)"), *AssetPath, *Asset->GetClass()->GetName());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetInfoTask);
	return FMCPResponse::Success(Request.Id, Result);
}

// ==========================================================================
// Phase 1.D — New management handlers
// ==========================================================================

namespace
{
	// Resolve an FAssetData from either a full object path ("/Game/Foo.Foo")
	// or a package path ("/Game/Foo").
	FAssetData ResolveAssetData(const FString& Path)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		FAssetData Data = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Path));
		if (!Data.IsValid())
		{
			// Try as package path.
			TArray<FAssetData> Found;
			AssetRegistry.GetAssetsByPackageName(FName(*Path), Found);
			if (Found.Num() > 0) return Found[0];
		}
		return Data;
	}
}

FMCPResponse FAssetService::HandleSyncToBrowser(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (!Request.Params->TryGetArrayField(TEXT("asset_paths"), PathsArr) || PathsArr->Num() == 0)
	{
		return InvalidParams(Request.Id, TEXT("Missing or empty 'asset_paths' (string array)"));
	}

	TArray<FString> Paths;
	for (const TSharedPtr<FJsonValue>& V : *PathsArr)
	{
		Paths.Add(V->AsString());
	}

	auto Task = [Paths]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		TArray<FAssetData> AssetDatas;
		TArray<FString> NotFound;
		for (const FString& P : Paths)
		{
			const FAssetData Data = ResolveAssetData(P);
			if (Data.IsValid())
			{
				AssetDatas.Add(Data);
			}
			else
			{
				NotFound.Add(P);
			}
		}

		if (AssetDatas.Num() == 0)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No valid asset paths resolved"));
			TArray<TSharedPtr<FJsonValue>> MissingJson;
			for (const FString& P : NotFound) MissingJson.Add(MakeShared<FJsonValueString>(P));
			Result->SetArrayField(TEXT("not_found"), MissingJson);
			return Result;
		}

		FContentBrowserModule& CBModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		CBModule.Get().SyncBrowserToAssets(AssetDatas);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("synced_count"), AssetDatas.Num());
		TArray<TSharedPtr<FJsonValue>> MissingJson;
		for (const FString& P : NotFound) MissingJson.Add(MakeShared<FJsonValueString>(P));
		Result->SetArrayField(TEXT("not_found"), MissingJson);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/sync_to_browser synced %d asset(s)"), AssetDatas.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleCreateFolder(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString DirectoryPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("directory_path"), DirectoryPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'directory_path' (e.g., /Game/NewFolder)"));
	}

	auto Task = [DirectoryPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		const bool bOk = UEditorAssetLibrary::MakeDirectory(DirectoryPath);
		Result->SetBoolField(TEXT("success"), bOk);
		Result->SetStringField(TEXT("directory_path"), DirectoryPath);
		if (!bOk)
		{
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("MakeDirectory failed for %s"), *DirectoryPath));
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: assets/create_folder failed for %s"), *DirectoryPath);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/create_folder created %s"), *DirectoryPath);
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleRenameAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString AssetPath, NewName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
	}
	if (!FMCPJson::ReadString(Request.Params, TEXT("new_name"), NewName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'new_name'"));
	}

	auto Task = [AssetPath, NewName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return Result;
		}

		// Preserve package path, change only the asset name.
		const FString PackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(TWeakObjectPtr<UObject>(Asset), PackagePath, NewName);
		const bool bOk = AssetTools.RenameAssets(RenameData);

		Result->SetBoolField(TEXT("success"), bOk);
		Result->SetStringField(TEXT("old_path"), AssetPath);
		Result->SetStringField(TEXT("new_package_path"), PackagePath);
		Result->SetStringField(TEXT("new_name"), NewName);
		if (!bOk)
		{
			Result->SetStringField(TEXT("error"), TEXT("RenameAssets returned false"));
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: assets/rename failed for %s -> %s"), *AssetPath, *NewName);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/rename %s -> %s"), *AssetPath, *NewName);
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleDeleteAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString AssetPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
	}

	bool bShowConfirmation = false;
	FMCPJson::ReadBool(Request.Params, TEXT("show_confirmation"), bShowConfirmation);

	auto Task = [AssetPath, bShowConfirmation]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		const FAssetData Data = ResolveAssetData(AssetPath);
		if (!Data.IsValid())
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return Result;
		}

		TArray<FAssetData> ToDelete;
		ToDelete.Add(Data);
		const int32 NumDeleted = ObjectTools::DeleteAssets(ToDelete, bShowConfirmation);

		Result->SetBoolField(TEXT("success"), NumDeleted > 0);
		Result->SetNumberField(TEXT("num_deleted"), NumDeleted);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		if (NumDeleted == 0)
		{
			Result->SetStringField(TEXT("error"), TEXT("DeleteAssets deleted zero assets (may be referenced or locked)"));
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: assets/delete deleted 0 for %s"), *AssetPath);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/delete removed %s"), *AssetPath);
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleMoveAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString SourcePath, DestinationPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("source_path"), SourcePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'source_path'"));
	}
	if (!FMCPJson::ReadString(Request.Params, TEXT("destination_path"), DestinationPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'destination_path' (full object path, e.g., /Game/New/Foo.Foo)"));
	}

	auto Task = [SourcePath, DestinationPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		// UEditorAssetLibrary::RenameAsset handles cross-directory moves.
		const bool bOk = UEditorAssetLibrary::RenameAsset(SourcePath, DestinationPath);

		Result->SetBoolField(TEXT("success"), bOk);
		Result->SetStringField(TEXT("source_path"), SourcePath);
		Result->SetStringField(TEXT("destination_path"), DestinationPath);
		if (!bOk)
		{
			Result->SetStringField(TEXT("error"), TEXT("RenameAsset failed — verify source exists and destination is valid"));
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: assets/move failed %s -> %s"), *SourcePath, *DestinationPath);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/move %s -> %s"), *SourcePath, *DestinationPath);
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleDuplicateAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString SourcePath, NewName, NewPackagePath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("source_path"), SourcePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'source_path'"));
	}
	if (!FMCPJson::ReadString(Request.Params, TEXT("new_name"), NewName))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'new_name'"));
	}
	FMCPJson::ReadString(Request.Params, TEXT("new_package_path"), NewPackagePath);

	auto Task = [SourcePath, NewName, NewPackagePath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UObject* SrcAsset = LoadObject<UObject>(nullptr, *SourcePath);
		if (!SrcAsset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *SourcePath));
			return Result;
		}

		const FString TargetPackagePath = NewPackagePath.IsEmpty()
			? FPackageName::GetLongPackagePath(SrcAsset->GetOutermost()->GetName())
			: NewPackagePath;

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* NewAsset = AssetTools.DuplicateAsset(NewName, TargetPackagePath, SrcAsset);

		if (!NewAsset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("DuplicateAsset returned null"));
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: assets/duplicate failed for %s"), *SourcePath);
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("source_path"), SourcePath);
		Result->SetStringField(TEXT("new_asset_path"), NewAsset->GetPathName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/duplicate %s -> %s"), *SourcePath, *NewAsset->GetPathName());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleSaveAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString AssetPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
	}

	bool bOnlyIfDirty = true;
	FMCPJson::ReadBool(Request.Params, TEXT("only_if_dirty"), bOnlyIfDirty);

	auto Task = [AssetPath, bOnlyIfDirty]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		const bool bOk = UEditorAssetLibrary::SaveAsset(AssetPath, bOnlyIfDirty);

		Result->SetBoolField(TEXT("success"), bOk);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
		if (!bOk)
		{
			Result->SetStringField(TEXT("error"), TEXT("SaveAsset returned false"));
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: assets/save failed for %s"), *AssetPath);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/save %s"), *AssetPath);
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleSetMetadata(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString AssetPath, Key, Value;
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
	}
	if (!FMCPJson::ReadString(Request.Params, TEXT("key"), Key))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'key'"));
	}
	if (!FMCPJson::ReadString(Request.Params, TEXT("value"), Value))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'value'"));
	}

	auto Task = [AssetPath, Key, Value]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return Result;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Asset has no outer package"));
			return Result;
		}

#if WITH_METADATA
		FMetaData& MetaData = Package->GetMetaData();
		MetaData.SetValue(Asset, FName(*Key), *Value);
		Package->MarkPackageDirty();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("key"), Key);
		Result->SetStringField(TEXT("value"), Value);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/set_metadata %s[%s] = %s"), *AssetPath, *Key, *Value);
#else
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Metadata support disabled in this build (WITH_METADATA=0)"));
#endif
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetMetadata(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	FString AssetPath, Key;
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path'"));
	}
	FMCPJson::ReadString(Request.Params, TEXT("key"), Key);

	auto Task = [AssetPath, Key]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return Result;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Asset has no outer package"));
			return Result;
		}

#if WITH_METADATA
		FMetaData& MetaData = Package->GetMetaData();

		if (!Key.IsEmpty())
		{
			// Single-key lookup.
			const FString& Value = MetaData.GetValue(Asset, FName(*Key));
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset_path"), AssetPath);
			Result->SetStringField(TEXT("key"), Key);
			Result->SetStringField(TEXT("value"), Value);
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/get_metadata %s[%s]"), *AssetPath, *Key);
		}
		else
		{
			// Whole-object dump.
			TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
			if (const TMap<FName, FString>* Map = FMetaData::GetMapForObject(Asset))
			{
				for (const TPair<FName, FString>& Pair : *Map)
				{
					Values->SetStringField(Pair.Key.ToString(), Pair.Value);
				}
			}
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset_path"), AssetPath);
			Result->SetObjectField(TEXT("metadata"), Values);
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/get_metadata %s (all keys)"), *AssetPath);
		}
#else
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Metadata support disabled in this build (WITH_METADATA=0)"));
#endif
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleValidateAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	TArray<FString> Paths;
	if (Request.Params->TryGetArrayField(TEXT("asset_paths"), PathsArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *PathsArr)
		{
			Paths.Add(V->AsString());
		}
	}
	else
	{
		FString SinglePath;
		if (FMCPJson::ReadString(Request.Params, TEXT("asset_path"), SinglePath))
		{
			Paths.Add(SinglePath);
		}
	}

	if (Paths.Num() == 0)
	{
		return InvalidParams(Request.Id, TEXT("Missing 'asset_path' (string) or 'asset_paths' (array)"));
	}

	auto Task = [Paths]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Attempt to locate UEditorValidatorSubsystem by class name without
		// hard-linking against the DataValidation module. If not available,
		// fall back to a best-effort load+IsValid check.
		UClass* SubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/DataValidation.EditorValidatorSubsystem"));
		UObject* Subsystem = nullptr;
		if (SubsystemClass && GEditor)
		{
			// GetEditorSubsystemBase takes TSubclassOf<UEditorSubsystem>.
			// Walk subsystems and find one whose class matches.
			TArray<UClass*> Dummy;
			// Simpler path: try direct engine subsystem array via TypedGetEditorSubsystem.
			// Since we don't have the concrete type, use reflection via TSubclassOf is awkward;
			// stick with the fallback path below.
			Subsystem = nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> PerAsset;
		int32 ValidCount = 0;
		int32 InvalidCount = 0;

		for (const FString& P : Paths)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), P);

			UObject* Asset = LoadObject<UObject>(nullptr, *P);
			if (!Asset)
			{
				Entry->SetStringField(TEXT("result"), TEXT("Invalid"));
				Entry->SetStringField(TEXT("error"), TEXT("Asset not found or failed to load"));
				++InvalidCount;
				PerAsset.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			const bool bLowLevelValid = Asset->IsValidLowLevel();
			UPackage* Pkg = Asset->GetOutermost();
			const bool bPackageValid = Pkg && !Pkg->HasAnyPackageFlags(PKG_CompiledIn);

			if (bLowLevelValid && bPackageValid)
			{
				Entry->SetStringField(TEXT("result"), TEXT("Valid"));
				++ValidCount;
			}
			else
			{
				Entry->SetStringField(TEXT("result"), TEXT("Invalid"));
				if (!bLowLevelValid) Entry->SetStringField(TEXT("error"), TEXT("IsValidLowLevel returned false"));
				else if (!bPackageValid) Entry->SetStringField(TEXT("error"), TEXT("Asset package is a compiled-in script package"));
				++InvalidCount;
			}

			TArray<TSharedPtr<FJsonValue>> EmptyArr;
			Entry->SetArrayField(TEXT("errors"), EmptyArr);
			Entry->SetArrayField(TEXT("warnings"), EmptyArr);
			PerAsset.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("valid_count"), ValidCount);
		Result->SetNumberField(TEXT("invalid_count"), InvalidCount);
		Result->SetArrayField(TEXT("results"), PerAsset);
		Result->SetStringField(TEXT("method"),
			TEXT("basic load + IsValidLowLevel check; DataValidation module not linked — rich validation unavailable"));

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: assets/validate — %d valid, %d invalid across %d path(s)"),
			ValidCount, InvalidCount, Paths.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FAssetService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// list
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list");
		Tool.Description = TEXT("List assets from the Asset Registry using cached metadata (no asset load). Returns {success, assets:[{name, path (object path), class (full /Script/Module.Class), package_name}], count, total_found}. "
			"Params: filter (object, optional) with class (string, FULL TopLevelAssetPath like /Script/Engine.StaticMesh - short names like 'StaticMesh' match nothing), path (string, content folder root like /Game/Meshes - searched recursively), max_results (integer, default 1000). "
			"Workflow: use assets/find for a name substring or assets/search for free-text across all fields; call before world/spawn_actor to discover available content. "
			"Warning: an empty filter enumerates the whole registry; always pass class and/or path to bound the result.");

		TSharedPtr<FJsonObject> FilterParam = MakeShared<FJsonObject>();
		FilterParam->SetStringField(TEXT("type"), TEXT("object"));
		FilterParam->SetStringField(TEXT("description"), TEXT("Optional filter: 'class' (full TopLevelAssetPath, e.g. /Script/Engine.StaticMesh), 'path' (content folder, recursive, e.g. /Game/Meshes), 'max_results' (integer, default 1000)"));
		Tool.Parameters->SetObjectField(TEXT("filter"), FilterParam);
		
		Tools.Add(Tool);
	}
	
	// find
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("find");
		Tool.Description = TEXT("Find assets whose short name contains the given substring (case-sensitive Contains). Returns {success, assets:[{name, path (object path), class}], count}, capped at 100 matches. "
			"Params: name (string, substring of the asset's short name, required). "
			"Workflow: narrower than assets/search (matches name only); use assets/get_info or assets/get_properties to inspect a specific match. "
			"Warning: scans every asset in the registry (no path bound) so it can be slow on large projects; results are silently truncated at 100.");

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Substring of the asset's short name (case-sensitive Contains match)"));
		Tool.Parameters->SetObjectField(TEXT("name"), NameParam);
		Tool.RequiredParams.Add(TEXT("name"));
		
		Tools.Add(Tool);
	}
	
	// get_properties
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_properties");
		Tool.Description = TEXT("Read an asset's cached Asset Registry metadata (no asset load). Returns {success, properties:{name, path (object path), class, package_name, package_path, tags:[{key, value}]}}. "
			"Params: asset_path (string, full object path like /Game/Foo.Foo, required). "
			"Workflow: cheap registry-tag introspection; prefer assets/get_info for type-specific bounds/materials/LODs (which loads the asset). "
			"Warning: 'tags' are Asset Registry tags only, not the editor metadata read/written by content_browser/get_metadata.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Full object path (e.g., /Game/Characters/Hero.Hero)"));
		Tool.Parameters->SetObjectField(TEXT("asset_path"), PathParam);
		Tool.RequiredParams.Add(TEXT("asset_path"));

		Tools.Add(Tool);
	}

	// search
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("search");
		Tool.Description = TEXT("Free-text search (case-insensitive) across asset short name, object path, and class. Returns {success, assets:[{name, path (object path), class}], count, query}. "
			"Params: query (string, required); max_results (integer, default 100). "
			"Workflow: broader than assets/find (matches any of name/path/class); follow with assets/get_info before world/spawn_actor. "
			"Warning: scans the entire registry with no path bound, so it can be slow; results stop at max_results.");

		TSharedPtr<FJsonObject> QueryParam = MakeShared<FJsonObject>();
		QueryParam->SetStringField(TEXT("type"), TEXT("string"));
		QueryParam->SetStringField(TEXT("description"), TEXT("Search query string, matched case-insensitively against name, path, and class"));
		Tool.Parameters->SetObjectField(TEXT("query"), QueryParam);
		Tool.RequiredParams.Add(TEXT("query"));

		TSharedPtr<FJsonObject> MaxParam = MakeShared<FJsonObject>();
		MaxParam->SetStringField(TEXT("type"), TEXT("integer"));
		MaxParam->SetStringField(TEXT("description"), TEXT("Maximum number of results (default 100)"));
		Tool.Parameters->SetObjectField(TEXT("max_results"), MaxParam);
		
		Tools.Add(Tool);
	}
	
	// get_bounds
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_bounds");
		Tool.Description = TEXT("Get a StaticMesh or SkeletalMesh local-space bounds (cm) and pivot offset for ground-aligned placement. Returns {success, asset_path, asset_class, mesh_type, bounds:{min, max, center, extent (half-size), size (full), pivot_offset (=-center), bottom_z (=-min.Z, Z delta to ADD so the base sits on the ground)}, num_lods, num_vertices/num_triangles or num_bones}. All values are cm in the mesh's local frame. "
			"Params: asset_path (string, /Game/... object path to a StaticMesh or SkeletalMesh, required). "
			"Workflow: call BEFORE world/spawn_actor; add bounds.bottom_z to the raycast-hit Z so the pivot lands the base on the ground. "
			"Warning: non-mesh assets return success=false with an error; bounds are local (pre-scale), not world-space.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Full object path to a StaticMesh or SkeletalMesh (e.g., /Game/Meshes/MyMesh.MyMesh)"));
		Tool.Parameters->SetObjectField(TEXT("asset_path"), PathParam);
		Tool.RequiredParams.Add(TEXT("asset_path"));
		
		Tools.Add(Tool);
	}
	
	// get_info
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_info");
		Tool.Description = TEXT("Load an asset and return type-specific details (sizes/bounds in cm). StaticMesh: {bounds, num_lods, num_vertices, num_triangles, num_sections, materials, num_materials, collision, nanite_enabled, lightmap_resolution}. SkeletalMesh: {bounds, num_bones, materials, num_lods}. Material: {material_domain, is_two_sided}. Blueprint: {blueprint_type, parent_class, generated_class}. Texture: {width, height, num_mips}. Other types return only {type:'Other', asset_name, asset_class}. "
			"Params: asset_path (string, full object path /Game/Foo.Foo, or /Game/BP/Foo.Foo_C for the generated class, required). "
			"Workflow: call before world/spawn_actor to confirm the asset; pair with assets/get_bounds for the pivot/bottom-Z offset. "
			"Warning: loads the asset (may trigger texture/shader compiles on first access); use assets/get_properties for a cheaper registry-only read.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Full object path (e.g., /Game/Meshes/MyMesh.MyMesh or /Game/BP/MyActor.MyActor_C)"));
		Tool.Parameters->SetObjectField(TEXT("asset_path"), PathParam);
		Tool.RequiredParams.Add(TEXT("asset_path"));

		Tools.Add(Tool);
	}

	// -- Phase 1.D: management tools --------------------------------------

	Tools.Add(FMCPToolBuilder(
			TEXT("sync_to_browser"),
			TEXT("Navigate and select assets in the Content Browser UI. Returns {success, synced_count, not_found:[paths]}. "
			     "Params: asset_paths (string[], required, full object paths e.g. /Game/Foo.Foo; package paths /Game/Foo also resolve). "
			     "Workflow: after assets/find or assets/search, call this to show the user what was matched. "
			     "Warning: UI-only, mutates nothing; paths that do not resolve are returned in not_found rather than failing. Skip in headless/cooked runs (no Content Browser)."))
		.RequiredArrayOfString(TEXT("asset_paths"), TEXT("Array of object paths (/Game/Foo.Foo) or package paths (/Game/Foo) to reveal"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("create_folder"),
			TEXT("Create a Content Browser folder (virtual /Game path, not an OS directory). Returns {success, directory_path}. "
			     "Params: directory_path (string, required, virtual content path e.g. /Game/MyNewFolder). "
			     "Workflow: call before assets/move, assets/duplicate, or asset_import when targeting a new folder. "
			     "Warning: idempotent - succeeds if the folder already exists; the folder is empty until an asset is placed and saved into it."))
		.RequiredString(TEXT("directory_path"), TEXT("Virtual content path to create, e.g. /Game/MyNewFolder"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("rename"),
			TEXT("Rename an asset in place, keeping its folder and fixing up references. Returns {success, old_path, new_package_path, new_name}. "
			     "Params: asset_path (string, required, existing object path /Game/Foo.Foo), new_name (string, required, bare new name with no path or extension). "
			     "Workflow: pair with assets/search or assets/find to confirm the new name is free; to change folder instead use assets/move. "
			     "Warning: mutates and dirties the package (save afterward); leaves a redirector at the old path until referencers are fixed up, which can block reuse of the old name."))
		.RequiredString(TEXT("asset_path"), TEXT("Existing asset object path, e.g. /Game/Foo.Foo"))
		.RequiredString(TEXT("new_name"),   TEXT("New bare asset name (no path, no extension)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("delete"),
			TEXT("Delete a single asset (resolves object or package path). Returns {success, num_deleted, asset_path}. "
			     "Params: asset_path (string, required, object or package path), show_confirmation (bool, optional, default false; true pops a modal dialog that blocks the game thread). "
			     "Workflow: run asset_deps/get_referencers first to audit inbound uses; clear or fix up referencers before deleting. "
			     "Warning: destructive; num_deleted is 0 (success=false) when the asset is referenced or locked. Deleting a referenced asset forces every referencer to recompile."))
		.RequiredString(TEXT("asset_path"),        TEXT("Asset object path or package path to delete"))
		.OptionalBool  (TEXT("show_confirmation"), TEXT("Show the interactive confirmation dialog; default false for scripted use"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("move"),
			TEXT("Move/rename an asset to a new folder via a full destination object path. Returns {success, source_path, destination_path}. "
			     "Params: source_path (string, required, existing object path /Game/Foo.Foo), destination_path (string, required, FULL destination object path including the asset name, e.g. /Game/New/Foo.Foo). "
			     "Workflow: call assets/create_folder first if the destination directory is new; to keep the folder and only change the name use assets/rename. "
			     "Warning: mutates and dirties packages (save afterward); leaves a redirector at the source until referencers are fixed up."))
		.RequiredString(TEXT("source_path"),      TEXT("Existing asset object path, e.g. /Game/Foo.Foo"))
		.RequiredString(TEXT("destination_path"), TEXT("Full destination object path including the asset name, e.g. /Game/NewFolder/Foo.Foo"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("duplicate"),
			TEXT("Duplicate an asset under a new name (and optional new folder). Returns {success, source_path, new_asset_path}. "
			     "Params: source_path (string, required, object path), new_name (string, required, bare name no path), new_package_path (string, optional, destination folder; defaults to the source folder). "
			     "Workflow: handy for material/blueprint variants; call assets/save (or content_browser/save) afterward to persist. "
			     "Warning: the copy is created unsaved (in-memory only) until you save it; duplicating a large material then editing it recompiles all its shader expressions - prefer editing the parent."))
		.RequiredString(TEXT("source_path"),      TEXT("Source asset object path, e.g. /Game/Foo.Foo"))
		.RequiredString(TEXT("new_name"),         TEXT("New bare asset name (no path)"))
		.OptionalString(TEXT("new_package_path"), TEXT("Optional destination folder; defaults to the source folder"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("save"),
			TEXT("Persist an asset's package to disk. Returns {success, asset_path, only_if_dirty}. "
			     "Params: asset_path (string, required, object path), only_if_dirty (bool, optional, default true; pass false to force a write even when clean). "
			     "Workflow: call once after assets/set_metadata, assets/duplicate, or any edit; do not loop saves. "
			     "Warning: saving a Material/MaterialInstance or Blueprint recompiles it (can take seconds-minutes); keep only_if_dirty=true so unchanged assets are skipped."))
		.RequiredString(TEXT("asset_path"),    TEXT("Asset object path to save"))
		.OptionalBool  (TEXT("only_if_dirty"), TEXT("Only save when the asset has unsaved changes; default true"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("set_metadata"),
			TEXT("Store a string key/value on the asset's editor metadata (package FMetaData map) and mark the package dirty. Returns {success, asset_path, key, value}. "
			     "Params: asset_path (string, required, object path), key (string, required, FName-compatible), value (string, required). "
			     "Workflow: call assets/save afterward to persist; read it back with assets/get_metadata. "
			     "Warning: requires WITH_METADATA (editor builds only; stripped from cooked builds where it returns success=false). Does not auto-save."))
		.RequiredString(TEXT("asset_path"), TEXT("Asset object path"))
		.RequiredString(TEXT("key"),        TEXT("Metadata key (FName)"))
		.RequiredString(TEXT("value"),      TEXT("Metadata value (string)"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("get_metadata"),
			TEXT("Read editor metadata from an asset's package FMetaData map. Returns {success, asset_path, key, value} for a single key, or {success, asset_path, metadata:{key:value}} when key is omitted. "
			     "Params: asset_path (string, required, object path), key (string, optional; omit to dump every entry). "
			     "Workflow: pair with assets/set_metadata to audit or migrate tags. "
			     "Warning: a missing key returns the empty string, not an error; requires WITH_METADATA (editor builds only)."))
		.RequiredString(TEXT("asset_path"), TEXT("Asset object path"))
		.OptionalString(TEXT("key"),        TEXT("Optional metadata key; omit to dump the full map"))
		.Build());

	Tools.Add(FMCPToolBuilder(
			TEXT("validate"),
			TEXT("Run a lightweight load + IsValidLowLevel check on one or more assets. Returns {success, valid_count, invalid_count, results:[{asset_path, result:'Valid'|'Invalid', error?, errors:[], warnings:[]}], method}. "
			     "Params: asset_path (string, single) OR asset_paths (string[], batch) - provide exactly one. "
			     "Workflow: use in a pre-submit pass to catch assets that fail to load. "
			     "Warning: the DataValidation module is not linked, so the per-validator errors/warnings arrays are always empty - this only confirms the asset loads, not data-correctness."))
		.OptionalString       (TEXT("asset_path"),  TEXT("Single asset object path"))
		.OptionalArrayOfString(TEXT("asset_paths"), TEXT("Batch of asset object paths"))
		.Build());

	return Tools;
}

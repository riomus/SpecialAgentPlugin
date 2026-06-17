// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/MaterialService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPRequestContext.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "StaticParameterSet.h"
#include "Engine/Texture.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"

FString FMaterialService::GetServiceDescription() const
{
	return TEXT("Material authoring - create UMaterial / UMaterialInstanceConstant assets and edit parameters");
}

FMCPResponse FMaterialService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("create")) return HandleCreate(Request);
	if (MethodName == TEXT("create_instance")) return HandleCreateInstance(Request);
	if (MethodName == TEXT("set_scalar_parameter")) return HandleSetScalarParameter(Request);
	if (MethodName == TEXT("set_vector_parameter")) return HandleSetVectorParameter(Request);
	if (MethodName == TEXT("set_texture_parameter")) return HandleSetTextureParameter(Request);
	if (MethodName == TEXT("set_static_switch")) return HandleSetStaticSwitch(Request);
	if (MethodName == TEXT("list_parameters")) return HandleListParameters(Request);
	if (MethodName == TEXT("get_parameters")) return HandleGetParameters(Request);

	return MethodNotFound(Request.Id, TEXT("material"), MethodName);
}

FMCPResponse FMaterialService::HandleCreate(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString PackagePath, AssetName;
	if (!FMCPJson::ReadString(Request.Params, TEXT("package_path"), PackagePath))
		return InvalidParams(Request.Id, TEXT("Missing 'package_path' (e.g. /Game/Materials)"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_name"), AssetName))
		return InvalidParams(Request.Id, TEXT("Missing 'asset_name'"));

	auto Task = [PackagePath, AssetName]() -> TSharedPtr<FJsonObject>
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Create Material")));

		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
		if (!NewAsset)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to create material at %s/%s"), *PackagePath, *AssetName));
		}

		NewAsset->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
		Result->SetStringField(TEXT("asset_name"), NewAsset->GetName());
		Result->SetStringField(TEXT("asset_class"), NewAsset->GetClass()->GetName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/create -> %s"), *NewAsset->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCreateInstance(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString PackagePath, AssetName, ParentPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("package_path"), PackagePath))
		return InvalidParams(Request.Id, TEXT("Missing 'package_path'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("asset_name"), AssetName))
		return InvalidParams(Request.Id, TEXT("Missing 'asset_name'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("parent_material"), ParentPath))
		return InvalidParams(Request.Id, TEXT("Missing 'parent_material' (UMaterialInterface path)"));

	auto Task = [PackagePath, AssetName, ParentPath]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
		if (!Parent) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load parent material: %s"), *ParentPath));

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Create Material Instance")));

		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Parent;

		UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
		if (!NewAsset)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to create material instance at %s/%s"), *PackagePath, *AssetName));
		}

		NewAsset->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
		Result->SetStringField(TEXT("asset_name"), NewAsset->GetName());
		Result->SetStringField(TEXT("parent"), Parent->GetPathName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/create_instance -> %s (parent %s)"),
			*NewAsset->GetPathName(), *Parent->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetScalarParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString InstancePath, ParamName;
	double Value = 0.0;
	if (!FMCPJson::ReadString(Request.Params, TEXT("instance_path"), InstancePath))
		return InvalidParams(Request.Id, TEXT("Missing 'instance_path'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("parameter_name"), ParamName))
		return InvalidParams(Request.Id, TEXT("Missing 'parameter_name'"));
	if (!FMCPJson::ReadNumber(Request.Params, TEXT("value"), Value))
		return InvalidParams(Request.Id, TEXT("Missing 'value' (number)"));

	auto Task = [InstancePath, ParamName, Value]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *InstancePath);
		if (!MIC) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UMaterialInstanceConstant: %s"), *InstancePath));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Scalar Parameter")));
		MIC->Modify();

		FMaterialParameterInfo Info(*ParamName);
		MIC->SetScalarParameterValueEditorOnly(Info, static_cast<float>(Value));
		MIC->PostEditChange();
		MIC->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("instance_path"), InstancePath);
		Result->SetStringField(TEXT("parameter_name"), ParamName);
		Result->SetNumberField(TEXT("value"), Value);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/set_scalar_parameter %s.%s = %f"),
			*InstancePath, *ParamName, Value);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetVectorParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString InstancePath, ParamName;
	FLinearColor Color;
	if (!FMCPJson::ReadString(Request.Params, TEXT("instance_path"), InstancePath))
		return InvalidParams(Request.Id, TEXT("Missing 'instance_path'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("parameter_name"), ParamName))
		return InvalidParams(Request.Id, TEXT("Missing 'parameter_name'"));
	if (!FMCPJson::ReadColor(Request.Params, TEXT("value"), Color))
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'value' (expected [R,G,B] or [R,G,B,A])"));

	auto Task = [InstancePath, ParamName, Color]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *InstancePath);
		if (!MIC) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UMaterialInstanceConstant: %s"), *InstancePath));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Vector Parameter")));
		MIC->Modify();

		FMaterialParameterInfo Info(*ParamName);
		MIC->SetVectorParameterValueEditorOnly(Info, Color);
		MIC->PostEditChange();
		MIC->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("instance_path"), InstancePath);
		Result->SetStringField(TEXT("parameter_name"), ParamName);
		FMCPJson::WriteColor(Result, TEXT("value"), Color);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/set_vector_parameter %s.%s"),
			*InstancePath, *ParamName);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetTextureParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString InstancePath, ParamName, TexturePath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("instance_path"), InstancePath))
		return InvalidParams(Request.Id, TEXT("Missing 'instance_path'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("parameter_name"), ParamName))
		return InvalidParams(Request.Id, TEXT("Missing 'parameter_name'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("texture_path"), TexturePath))
		return InvalidParams(Request.Id, TEXT("Missing 'texture_path'"));

	auto Task = [InstancePath, ParamName, TexturePath]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *InstancePath);
		if (!MIC) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UMaterialInstanceConstant: %s"), *InstancePath));

		UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
		if (!Texture) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load texture: %s"), *TexturePath));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Texture Parameter")));
		MIC->Modify();

		FMaterialParameterInfo Info(*ParamName);
		MIC->SetTextureParameterValueEditorOnly(Info, Texture);
		MIC->PostEditChange();
		MIC->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("instance_path"), InstancePath);
		Result->SetStringField(TEXT("parameter_name"), ParamName);
		Result->SetStringField(TEXT("texture_path"), Texture->GetPathName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/set_texture_parameter %s.%s = %s"),
			*InstancePath, *ParamName, *Texture->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetStaticSwitch(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString InstancePath, ParamName;
	bool Value = false;
	if (!FMCPJson::ReadString(Request.Params, TEXT("instance_path"), InstancePath))
		return InvalidParams(Request.Id, TEXT("Missing 'instance_path'"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("parameter_name"), ParamName))
		return InvalidParams(Request.Id, TEXT("Missing 'parameter_name'"));
	if (!FMCPJson::ReadBool(Request.Params, TEXT("value"), Value))
		return InvalidParams(Request.Id, TEXT("Missing 'value' (boolean)"));

	auto Task = [InstancePath, ParamName, Value]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInstanceConstant* MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *InstancePath);
		if (!MIC) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UMaterialInstanceConstant: %s"), *InstancePath));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Static Switch")));
		MIC->Modify();

		FStaticParameterSet StaticParams = MIC->GetStaticParameters();

		FMaterialParameterInfo Info(*ParamName);
		bool bFound = false;
		for (FStaticSwitchParameter& P : StaticParams.StaticSwitchParameters)
		{
			if (P.ParameterInfo == Info)
			{
				P.Value = Value;
				P.bOverride = true;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FStaticSwitchParameter NewSwitch;
			NewSwitch.ParameterInfo = Info;
			NewSwitch.Value = Value;
			NewSwitch.bOverride = true;
			NewSwitch.ExpressionGUID = FGuid::NewGuid();
			StaticParams.StaticSwitchParameters.Add(NewSwitch);
		}

		MIC->UpdateStaticPermutation(StaticParams);
		MIC->PostEditChange();
		MIC->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("instance_path"), InstancePath);
		Result->SetStringField(TEXT("parameter_name"), ParamName);
		Result->SetBoolField(TEXT("value"), Value);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/set_static_switch %s.%s = %s"),
			*InstancePath, *ParamName, Value ? TEXT("true") : TEXT("false"));
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleListParameters(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString MaterialPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path' (UMaterialInterface path)"));

	auto Task = [MaterialPath]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UMaterialInterface: %s"), *MaterialPath));

		TArray<FMaterialParameterInfo> ScalarInfo;  TArray<FGuid> ScalarIds;
		TArray<FMaterialParameterInfo> VectorInfo;  TArray<FGuid> VectorIds;
		TArray<FMaterialParameterInfo> TextureInfo; TArray<FGuid> TextureIds;
		Material->GetAllScalarParameterInfo(ScalarInfo, ScalarIds);
		Material->GetAllVectorParameterInfo(VectorInfo, VectorIds);
		Material->GetAllTextureParameterInfo(TextureInfo, TextureIds);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("material_path"), MaterialPath);

		TArray<TSharedPtr<FJsonValue>> ScalarArr;
		for (const FMaterialParameterInfo& P : ScalarInfo) ScalarArr.Add(MakeShared<FJsonValueString>(P.Name.ToString()));
		Result->SetArrayField(TEXT("scalar_parameters"), ScalarArr);

		TArray<TSharedPtr<FJsonValue>> VectorArr;
		for (const FMaterialParameterInfo& P : VectorInfo) VectorArr.Add(MakeShared<FJsonValueString>(P.Name.ToString()));
		Result->SetArrayField(TEXT("vector_parameters"), VectorArr);

		TArray<TSharedPtr<FJsonValue>> TextureArr;
		for (const FMaterialParameterInfo& P : TextureInfo) TextureArr.Add(MakeShared<FJsonValueString>(P.Name.ToString()));
		Result->SetArrayField(TEXT("texture_parameters"), TextureArr);

		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleGetParameters(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString MaterialPath;
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path'"));

	auto Task = [MaterialPath]() -> TSharedPtr<FJsonObject>
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load UMaterialInterface: %s"), *MaterialPath));

		TArray<FMaterialParameterInfo> ScalarInfo;  TArray<FGuid> ScalarIds;
		TArray<FMaterialParameterInfo> VectorInfo;  TArray<FGuid> VectorIds;
		TArray<FMaterialParameterInfo> TextureInfo; TArray<FGuid> TextureIds;
		Material->GetAllScalarParameterInfo(ScalarInfo, ScalarIds);
		Material->GetAllVectorParameterInfo(VectorInfo, VectorIds);
		Material->GetAllTextureParameterInfo(TextureInfo, TextureIds);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("material_path"), MaterialPath);

		TArray<TSharedPtr<FJsonValue>> ScalarArr;
		for (const FMaterialParameterInfo& P : ScalarInfo)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), P.Name.ToString());
			float V = 0.0f;
			if (Material->GetScalarParameterValue(P, V))
			{
				Obj->SetNumberField(TEXT("value"), V);
			}
			ScalarArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("scalar_parameters"), ScalarArr);

		TArray<TSharedPtr<FJsonValue>> VectorArr;
		for (const FMaterialParameterInfo& P : VectorInfo)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), P.Name.ToString());
			FLinearColor V;
			if (Material->GetVectorParameterValue(P, V))
			{
				FMCPJson::WriteColor(Obj, TEXT("value"), V);
			}
			VectorArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("vector_parameters"), VectorArr);

		TArray<TSharedPtr<FJsonValue>> TextureArr;
		for (const FMaterialParameterInfo& P : TextureInfo)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), P.Name.ToString());
			UTexture* Tex = nullptr;
			if (Material->GetTextureParameterValue(P, Tex) && Tex)
			{
				Obj->SetStringField(TEXT("value"), Tex->GetPathName());
			}
			TextureArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("texture_parameters"), TextureArr);

		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

TArray<FMCPToolInfo> FMaterialService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(TEXT("create"),
		TEXT("Create a new empty UMaterial asset in the Content Browser via AssetTools. "
			 "Returns {asset_path, asset_name, asset_class}; asset_path is the object path you feed to list_parameters/get_parameters. "
			 "Params: package_path (string, virtual content folder like /Game/Materials, required), asset_name (string, bare name with no extension, required). "
			 "Workflow: create -> open in the Material Editor to author the graph and add parameters -> create_instance to spawn parametrized variants. "
			 "Warning: CreateAsset fails (returns error) if an asset of that name already exists, so pick a unique asset_name; the new material is unsaved (in-memory) until you save it explicitly."))
		.RequiredString(TEXT("package_path"), TEXT("Virtual content folder, e.g. /Game/Materials (no OS path, no extension)"))
		.RequiredString(TEXT("asset_name"), TEXT("Bare name for the new material asset (must be unique in the folder)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("create_instance"),
		TEXT("Create a UMaterialInstanceConstant (editor/persistent MIC, not a runtime MID) parented to an existing UMaterialInterface. "
			 "Returns {asset_path, asset_name, parent}; pass asset_path as instance_path to the set_* parameter tools. "
			 "Params: package_path (string, /Game/... folder, required), asset_name (string, bare name, required), parent_material (string, object path Pkg.Asset like /Game/Materials/M_Base.M_Base, required). "
			 "Workflow: author a parametrized UMaterial -> create_instance from it -> set_scalar/vector/texture/static_switch to override per-instance params. "
			 "Warning: parent_material must load (fails otherwise); CreateAsset fails on a duplicate asset_name; the MIC is left unsaved (in-memory) until saved explicitly."))
		.RequiredString(TEXT("package_path"), TEXT("Virtual content folder, e.g. /Game/Materials"))
		.RequiredString(TEXT("asset_name"), TEXT("Bare name for the new material instance asset (must be unique)"))
		.RequiredString(TEXT("parent_material"), TEXT("Object path of parent UMaterialInterface, e.g. /Game/Materials/M_Base.M_Base"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_scalar_parameter"),
		TEXT("Override a scalar (float) parameter on a UMaterialInstanceConstant asset (editor-only set, cheap, no shader recompile). "
			 "Returns {instance_path, parameter_name, value}. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, must match a scalar param on the parent, required), value (number, required). "
			 "Workflow: list_parameters first to discover valid scalar names -> set_scalar_parameter -> save the asset to persist. "
			 "Warning: an unknown parameter_name is silently accepted (no error) but has no visual effect; this only marks the package dirty (MarkPackageDirty), it does NOT save to disk."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path, e.g. /Game/MI_Foo.MI_Foo"))
		.RequiredString(TEXT("parameter_name"), TEXT("Scalar parameter name as defined in the parent material"))
		.RequiredNumber(TEXT("value"), TEXT("New scalar value (unitless float)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_vector_parameter"),
		TEXT("Override a vector parameter on a UMaterialInstanceConstant asset with a linear color (editor-only, cheap, no recompile). "
			 "Returns {instance_path, parameter_name, value}. Value is a FLinearColor in linear space (channels 0..1, may exceed 1 for HDR) - NOT a position in cm and NOT degrees. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, required), value (array [R,G,B] or [R,G,B,A], required). "
			 "Workflow: list_parameters to discover vector names; pair with set_scalar/set_texture for a consistent look. "
			 "Warning: alpha defaults to 1.0 when omitted; unknown parameter_name is silently ignored; only marks the package dirty - save the asset to persist."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Vector parameter name as defined in the parent material"))
		.RequiredColor(TEXT("value"), TEXT("Linear color [R,G,B] or [R,G,B,A], channels 0..1 (HDR may exceed 1)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_texture_parameter"),
		TEXT("Override a texture parameter on a UMaterialInstanceConstant asset (editor-only set). "
			 "Returns {instance_path, parameter_name, texture_path}. The handler loads the texture object internally, so you pass a content path string, not a loaded object. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, required), texture_path (string, UTexture object path Pkg.Asset, required). "
			 "Workflow: use assets/find to locate textures -> set_texture_parameter -> save the asset to persist. "
			 "Warning: texture_path must load (fails otherwise); unknown parameter_name is silently ignored; only marks the package dirty, does not save; texture compression / shader work can make the swap non-instant."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Texture parameter name as defined in the parent material"))
		.RequiredString(TEXT("texture_path"), TEXT("UTexture object path, e.g. /Game/Tex/T_Grass.T_Grass"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_static_switch"),
		TEXT("Override a static switch on a UMaterialInstanceConstant and rebuild its static permutation. "
			 "Returns {instance_path, parameter_name, value}. If the switch isn't already overridden the handler adds the override entry. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, required), value (boolean, required). "
			 "Workflow: introspect static switch names in the parent Material Editor first; batch multiple switch sets, then save once. "
			 "Warning: changing a static switch forces a FULL shader-permutation recompile (UpdateStaticPermutation) - expensive, can take several seconds, so avoid looping; re-applying the same value still rebuilds; only marks the package dirty, does not save to disk."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Static switch parameter name as defined in the parent material"))
		.RequiredBool(TEXT("value"), TEXT("New static switch value (true/false)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_parameters"),
		TEXT("List scalar/vector/texture parameter NAMES (names only, no values) on a UMaterial or UMaterialInstanceConstant. Read-only. "
			 "Returns {material_path, scalar_parameters[], vector_parameters[], texture_parameters[]} where each array holds parameter-name strings. "
			 "Params: material_path (string, object path of a UMaterialInterface - UMaterial or UMaterialInstanceConstant, required). "
			 "Workflow: call before any set_*_parameter to discover valid names; use get_parameters when you also need current values. "
			 "Warning: static-switch parameters are NOT included in any of these arrays; material_path must load or the call errors."))
		.RequiredString(TEXT("material_path"), TEXT("Object path of a UMaterial or UMaterialInstanceConstant"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("get_parameters"),
		TEXT("List scalar/vector/texture parameters AND their current values on a UMaterial or UMaterialInstanceConstant. Read-only. "
			 "Returns {material_path, scalar_parameters[{name, value:number}], vector_parameters[{name, value:[R,G,B,A] linear}], texture_parameters[{name, value:texture object path}]}. "
			 "Params: material_path (string, UMaterialInterface object path, required). "
			 "Workflow: call after set_*_parameter to verify the applied value; use list_parameters when you only need names. "
			 "Warning: values are the EFFECTIVE values (instance override or parent fallback), not override-only; static switches are not reported; material_path must load."))
		.RequiredString(TEXT("material_path"), TEXT("Object path of a UMaterial or UMaterialInstanceConstant"))
		.Build());

	return Tools;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/MaterialService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPToolBuilder.h"

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

FString FMaterialService::GetServiceDescription() const
{
	return TEXT("Material authoring - create UMaterial / UMaterialInstanceConstant assets and edit parameters");
}

FMCPResponse FMaterialService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
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

		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
		if (!NewAsset)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to create material at %s/%s"), *PackagePath, *AssetName));
		}

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

		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Parent;

		UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
		if (!NewAsset)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to create material instance at %s/%s"), *PackagePath, *AssetName));
		}

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
		TEXT("Create UMaterial asset. Produces a new empty UMaterial in the Content Browser.\n"
			 "Params: package_path (string, content path like /Game/Materials), asset_name (string).\n"
			 "Workflow: open in Material Editor in UE to author the graph; use create_instance for param variants.\n"
			 "Warning: AssetTools refuses duplicates — pick a unique asset_name."))
		.RequiredString(TEXT("package_path"), TEXT("Content-browser folder path (e.g. /Game/Materials)"))
		.RequiredString(TEXT("asset_name"), TEXT("Name for the new material asset"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("create_instance"),
		TEXT("Create UMaterialInstanceConstant parented to an existing UMaterialInterface.\n"
			 "Params: package_path (string), asset_name (string), parent_material (string, object path).\n"
			 "Workflow: author a parametrized UMaterial first, then spawn instances to vary scalar/vector/texture params.\n"
			 "Warning: parent must exist and compile."))
		.RequiredString(TEXT("package_path"), TEXT("Content-browser folder path"))
		.RequiredString(TEXT("asset_name"), TEXT("Name for the new material instance asset"))
		.RequiredString(TEXT("parent_material"), TEXT("Object path of parent (e.g. /Game/Materials/M_Base.M_Base)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_scalar_parameter"),
		TEXT("Override a scalar parameter on a UMaterialInstanceConstant.\n"
			 "Params: instance_path (string), parameter_name (string), value (number).\n"
			 "Workflow: list_parameters first to discover valid names.\n"
			 "Warning: editor-only; save the asset afterwards."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Scalar parameter name as defined in the parent"))
		.RequiredNumber(TEXT("value"), TEXT("New scalar value"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_vector_parameter"),
		TEXT("Override a vector parameter on a UMaterialInstanceConstant with a linear color.\n"
			 "Params: instance_path (string), parameter_name (string), value (array [R,G,B] or [R,G,B,A]).\n"
			 "Workflow: list_parameters first; pair with scalar/texture for a consistent look.\n"
			 "Warning: alpha defaults to 1.0 when omitted."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Vector parameter name"))
		.RequiredColor(TEXT("value"), TEXT("Linear color [R,G,B] or [R,G,B,A]"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_texture_parameter"),
		TEXT("Override a texture parameter on a UMaterialInstanceConstant.\n"
			 "Params: instance_path (string), parameter_name (string), texture_path (string, UTexture object path).\n"
			 "Workflow: use assets/find to locate textures.\n"
			 "Warning: texture compression / shader compile can make the swap non-instant."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Texture parameter name"))
		.RequiredString(TEXT("texture_path"), TEXT("UTexture object path (e.g. /Game/Tex/T_Grass.T_Grass)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_static_switch"),
		TEXT("Override a static switch on a UMaterialInstanceConstant and rebuild the static permutation.\n"
			 "Params: instance_path (string), parameter_name (string), value (boolean).\n"
			 "Workflow: introspect static switches in the parent material editor before calling.\n"
			 "Warning: triggers shader recompile for the instance; can take several seconds."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Static switch parameter name"))
		.RequiredBool(TEXT("value"), TEXT("New switch value"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_parameters"),
		TEXT("List scalar/vector/texture parameter NAMES on a UMaterialInterface.\n"
			 "Params: material_path (string, UMaterial or UMaterialInstanceConstant path).\n"
			 "Workflow: call before any set_*_parameter to discover valid names.\n"
			 "Warning: static-switch parameters are not listed; use get_parameters for effective values."))
		.RequiredString(TEXT("material_path"), TEXT("Object path of UMaterial or UMaterialInstanceConstant"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("get_parameters"),
		TEXT("List parameters AND current values on a UMaterialInterface.\n"
			 "Params: material_path (string).\n"
			 "Workflow: diff against list_parameters to identify overrides.\n"
			 "Warning: reflects effective value after parent fallback, not override-only."))
		.RequiredString(TEXT("material_path"), TEXT("Object path of UMaterial or UMaterialInstanceConstant"))
		.Build());

	return Tools;
}

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
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "MaterialEditingLibrary.h"
#include "SceneTypes.h"
#include "Engine/EngineTypes.h"
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
	if (MethodName == TEXT("add_expression")) return HandleAddExpression(Request);
	if (MethodName == TEXT("connect_expression")) return HandleConnectExpression(Request);
	if (MethodName == TEXT("set_base_properties")) return HandleSetBaseProperties(Request);
	if (MethodName == TEXT("recompile")) return HandleRecompile(Request);

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

		FMaterialParameterInfo Info(*ParamName);
		float ExistingValue = 0.0f;
		if (!MIC->GetScalarParameterValue(Info, ExistingValue))
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Parameter '%s' not found on material instance %s"), *ParamName, *InstancePath));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Scalar Parameter")));
		MIC->Modify();

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
		FLinearColor ExistingValue;
		if (!MIC->GetVectorParameterValue(Info, ExistingValue))
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Parameter '%s' not found on material instance %s"), *ParamName, *InstancePath));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Vector Parameter")));
		MIC->Modify();

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
		UTexture* ExistingValue = nullptr;
		if (!MIC->GetTextureParameterValue(Info, ExistingValue))
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Parameter '%s' not found on material instance %s"), *ParamName, *InstancePath));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Texture Parameter")));
		MIC->Modify();

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

		FMaterialParameterInfo Info(*ParamName);
		bool ExistingValue = false;
		FGuid ExistingGuid;
		if (!MIC->GetStaticSwitchParameterValue(Info, ExistingValue, ExistingGuid))
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Parameter '%s' not found on material instance %s"), *ParamName, *InstancePath));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Static Switch")));
		MIC->Modify();

		FStaticParameterSet StaticParams = MIC->GetStaticParameters();

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

// Map an expression class short name to its UClass*. Returns nullptr if not one
// of the supported authoring expression types.
static UClass* ResolveMaterialExpressionClass(const FString& Name)
{
	if (Name == TEXT("MaterialExpressionTextureSample"))     return UMaterialExpressionTextureSample::StaticClass();
	if (Name == TEXT("MaterialExpressionScalarParameter"))   return UMaterialExpressionScalarParameter::StaticClass();
	if (Name == TEXT("MaterialExpressionVectorParameter"))   return UMaterialExpressionVectorParameter::StaticClass();
	if (Name == TEXT("MaterialExpressionConstant"))          return UMaterialExpressionConstant::StaticClass();
	if (Name == TEXT("MaterialExpressionMultiply"))          return UMaterialExpressionMultiply::StaticClass();
	if (Name == TEXT("MaterialExpressionAdd"))               return UMaterialExpressionAdd::StaticClass();
	if (Name == TEXT("MaterialExpressionTextureCoordinate")) return UMaterialExpressionTextureCoordinate::StaticClass();
	return nullptr;
}

// Map a friendly material property name to EMaterialProperty. Returns true and
// fills Out on success. MP_WorldPositionOffset is intentionally NOT exposed.
static bool ResolveMaterialProperty(const FString& Name, EMaterialProperty& Out)
{
	if (Name == TEXT("BaseColor"))         { Out = MP_BaseColor;        return true; }
	if (Name == TEXT("Metallic"))          { Out = MP_Metallic;         return true; }
	if (Name == TEXT("Specular"))          { Out = MP_Specular;         return true; }
	if (Name == TEXT("Roughness"))         { Out = MP_Roughness;        return true; }
	if (Name == TEXT("Normal"))            { Out = MP_Normal;           return true; }
	if (Name == TEXT("EmissiveColor"))     { Out = MP_EmissiveColor;    return true; }
	if (Name == TEXT("Opacity"))           { Out = MP_Opacity;          return true; }
	if (Name == TEXT("OpacityMask"))       { Out = MP_OpacityMask;      return true; }
	if (Name == TEXT("AmbientOcclusion"))  { Out = MP_AmbientOcclusion; return true; }
	return false;
}

static bool ResolveBlendMode(const FString& Name, EBlendMode& Out)
{
	if (Name == TEXT("Opaque"))         { Out = BLEND_Opaque;         return true; }
	if (Name == TEXT("Masked"))         { Out = BLEND_Masked;         return true; }
	if (Name == TEXT("Translucent"))    { Out = BLEND_Translucent;    return true; }
	if (Name == TEXT("Additive"))       { Out = BLEND_Additive;       return true; }
	if (Name == TEXT("Modulate"))       { Out = BLEND_Modulate;       return true; }
	if (Name == TEXT("AlphaComposite")) { Out = BLEND_AlphaComposite; return true; }
	if (Name == TEXT("AlphaHoldout"))   { Out = BLEND_AlphaHoldout;   return true; }
	return false;
}

static bool ResolveShadingModel(const FString& Name, EMaterialShadingModel& Out)
{
	if (Name == TEXT("Unlit"))             { Out = MSM_Unlit;             return true; }
	if (Name == TEXT("DefaultLit"))        { Out = MSM_DefaultLit;        return true; }
	if (Name == TEXT("Subsurface"))        { Out = MSM_Subsurface;        return true; }
	if (Name == TEXT("PreintegratedSkin")) { Out = MSM_PreintegratedSkin; return true; }
	if (Name == TEXT("ClearCoat"))         { Out = MSM_ClearCoat;         return true; }
	if (Name == TEXT("SubsurfaceProfile")) { Out = MSM_SubsurfaceProfile; return true; }
	if (Name == TEXT("TwoSidedFoliage"))   { Out = MSM_TwoSidedFoliage;   return true; }
	if (Name == TEXT("Hair"))              { Out = MSM_Hair;              return true; }
	if (Name == TEXT("Cloth"))             { Out = MSM_Cloth;             return true; }
	if (Name == TEXT("Eye"))               { Out = MSM_Eye;               return true; }
	if (Name == TEXT("SingleLayerWater"))  { Out = MSM_SingleLayerWater;  return true; }
	if (Name == TEXT("ThinTranslucent"))   { Out = MSM_ThinTranslucent;   return true; }
	return false;
}

// Re-find a material expression by its GetName() on a base UMaterial. Handles
// do not persist across MCP calls, so callers always pass names.
static UMaterialExpression* FindExpressionByName(UMaterial* Material, const FString& Name)
{
	for (const TObjectPtr<UMaterialExpression>& Expr : Material->GetExpressions())
	{
		if (Expr && Expr->GetName() == Name)
		{
			return Expr.Get();
		}
	}
	return nullptr;
}

FMCPResponse FMaterialService::HandleAddExpression(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString MaterialPath, ExpressionClass;
	int32 NodePosX = 0, NodePosY = 0;
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path' (base UMaterial object path)"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("expression_class"), ExpressionClass))
		return InvalidParams(Request.Id, TEXT("Missing 'expression_class' (e.g. MaterialExpressionScalarParameter)"));
	FMCPJson::ReadInteger(Request.Params, TEXT("node_pos_x"), NodePosX);
	FMCPJson::ReadInteger(Request.Params, TEXT("node_pos_y"), NodePosY);

	auto Task = [MaterialPath, ExpressionClass, NodePosX, NodePosY]() -> TSharedPtr<FJsonObject>
	{
		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *MaterialPath);
		if (!Mat) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load base UMaterial: %s"), *MaterialPath));

		UClass* Class = ResolveMaterialExpressionClass(ExpressionClass);
		if (!Class)
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Unsupported expression_class '%s' (supported: MaterialExpression TextureSample/ScalarParameter/VectorParameter/Constant/Multiply/Add/TextureCoordinate)"),
				*ExpressionClass));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Add Material Expression")));
		Mat->Modify();

		UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(Mat, Class, NodePosX, NodePosY);
		if (!NewExpr)
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("CreateMaterialExpression returned null for class %s on %s"), *ExpressionClass, *MaterialPath));
		}

		Mat->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("material_path"), MaterialPath);
		Result->SetStringField(TEXT("expression_name"), NewExpr->GetName());
		Result->SetStringField(TEXT("expression_class"), ExpressionClass);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/add_expression %s -> %s (%s)"),
			*MaterialPath, *NewExpr->GetName(), *ExpressionClass);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleConnectExpression(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString MaterialPath, FromName, FromOutput, ToName, ToInput, ToProperty;
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path' (base UMaterial object path)"));
	if (!FMCPJson::ReadString(Request.Params, TEXT("from_expression"), FromName))
		return InvalidParams(Request.Id, TEXT("Missing 'from_expression' (source expression name)"));
	FMCPJson::ReadString(Request.Params, TEXT("from_output"), FromOutput);
	FMCPJson::ReadString(Request.Params, TEXT("to_expression"), ToName);
	FMCPJson::ReadString(Request.Params, TEXT("to_input"), ToInput);
	const bool bHasProperty = FMCPJson::ReadString(Request.Params, TEXT("to_property"), ToProperty);

	if (!bHasProperty && ToName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Provide either 'to_property' (material input) or 'to_expression' (target node name)"));
	}

	auto Task = [MaterialPath, FromName, FromOutput, ToName, ToInput, ToProperty, bHasProperty]() -> TSharedPtr<FJsonObject>
	{
		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *MaterialPath);
		if (!Mat) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load base UMaterial: %s"), *MaterialPath));

		UMaterialExpression* From = FindExpressionByName(Mat, FromName);
		if (!From) return FMCPJson::MakeError(FString::Printf(TEXT("from_expression '%s' not found on %s"), *FromName, *MaterialPath));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Connect Material Expression")));
		Mat->Modify();

		bool bConnected = false;
		if (bHasProperty)
		{
			EMaterialProperty Property = MP_BaseColor;
			if (!ResolveMaterialProperty(ToProperty, Property))
			{
				return FMCPJson::MakeError(FString::Printf(
					TEXT("Unsupported to_property '%s' (supported: BaseColor/Metallic/Specular/Roughness/Normal/EmissiveColor/Opacity/OpacityMask/AmbientOcclusion; WorldPositionOffset is hidden and rejected)"),
					*ToProperty));
			}
			bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Property);
		}
		else
		{
			UMaterialExpression* To = FindExpressionByName(Mat, ToName);
			if (!To) return FMCPJson::MakeError(FString::Printf(TEXT("to_expression '%s' not found on %s"), *ToName, *MaterialPath));
			bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput);
		}

		Mat->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("material_path"), MaterialPath);
		Result->SetStringField(TEXT("from_expression"), FromName);
		if (bHasProperty) Result->SetStringField(TEXT("to_property"), ToProperty);
		else              Result->SetStringField(TEXT("to_expression"), ToName);
		Result->SetBoolField(TEXT("connected"), bConnected);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/connect_expression %s.%s -> %s (connected=%s)"),
			*MaterialPath, *FromName, bHasProperty ? *ToProperty : *ToName, bConnected ? TEXT("true") : TEXT("false"));
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetBaseProperties(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString MaterialPath, BlendModeStr, ShadingModelStr;
	bool TwoSided = false;
	double ClipValue = 0.0;
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path' (base UMaterial object path)"));
	const bool bHasBlendMode    = FMCPJson::ReadString(Request.Params, TEXT("blend_mode"), BlendModeStr);
	const bool bHasShadingModel = FMCPJson::ReadString(Request.Params, TEXT("shading_model"), ShadingModelStr);
	const bool bHasTwoSided     = FMCPJson::ReadBool(Request.Params, TEXT("two_sided"), TwoSided);
	const bool bHasClipValue    = FMCPJson::ReadNumber(Request.Params, TEXT("opacity_mask_clip_value"), ClipValue);

	if (!bHasBlendMode && !bHasShadingModel && !bHasTwoSided && !bHasClipValue)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one of: blend_mode, shading_model, two_sided, opacity_mask_clip_value"));
	}

	// Resolve enum strings up front so an invalid value fails before touching the asset.
	EBlendMode NewBlendMode = BLEND_Opaque;
	if (bHasBlendMode && !ResolveBlendMode(BlendModeStr, NewBlendMode))
	{
		return InvalidParams(Request.Id, FString::Printf(
			TEXT("Unsupported blend_mode '%s' (Opaque/Masked/Translucent/Additive/Modulate/AlphaComposite/AlphaHoldout)"), *BlendModeStr));
	}
	EMaterialShadingModel NewShadingModel = MSM_DefaultLit;
	if (bHasShadingModel && !ResolveShadingModel(ShadingModelStr, NewShadingModel))
	{
		return InvalidParams(Request.Id, FString::Printf(
			TEXT("Unsupported shading_model '%s' (Unlit/DefaultLit/Subsurface/PreintegratedSkin/ClearCoat/SubsurfaceProfile/TwoSidedFoliage/Hair/Cloth/Eye/SingleLayerWater/ThinTranslucent)"), *ShadingModelStr));
	}

	auto Task = [MaterialPath, bHasBlendMode, NewBlendMode, bHasShadingModel, NewShadingModel,
		bHasTwoSided, TwoSided, bHasClipValue, ClipValue]() -> TSharedPtr<FJsonObject>
	{
		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *MaterialPath);
		if (!Mat) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load base UMaterial: %s"), *MaterialPath));

		// Idempotency: only mutate fields whose target differs from the current value.
		bool bChanged = false;
		if (bHasBlendMode && Mat->BlendMode != NewBlendMode) bChanged = true;
		if (bHasShadingModel && !Mat->GetShadingModels().HasShadingModel(NewShadingModel)) bChanged = true;
		if (bHasTwoSided && (Mat->TwoSided != 0) != TwoSided) bChanged = true;
		if (bHasClipValue && Mat->OpacityMaskClipValue != static_cast<float>(ClipValue)) bChanged = true;

		if (bChanged)
		{
			const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Set Material Base Properties")));
			Mat->Modify();

			if (bHasBlendMode)    Mat->BlendMode = NewBlendMode;
			if (bHasShadingModel) Mat->SetShadingModel(NewShadingModel);
			if (bHasTwoSided)     Mat->TwoSided = TwoSided ? 1 : 0;
			if (bHasClipValue)    Mat->OpacityMaskClipValue = static_cast<float>(ClipValue);

			Mat->PostEditChange();
			Mat->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("material_path"), MaterialPath);
		Result->SetBoolField(TEXT("changed"), bChanged);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/set_base_properties %s (changed=%s)"),
			*MaterialPath, bChanged ? TEXT("true") : TEXT("false"));
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRecompile(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params"));

	FString MaterialPath;
	bool bLayout = false;
	if (!FMCPJson::ReadString(Request.Params, TEXT("material_path"), MaterialPath))
		return InvalidParams(Request.Id, TEXT("Missing 'material_path' (base UMaterial object path)"));
	FMCPJson::ReadBool(Request.Params, TEXT("layout_expressions"), bLayout);

	auto Task = [MaterialPath, bLayout]() -> TSharedPtr<FJsonObject>
	{
		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *MaterialPath);
		if (!Mat) return FMCPJson::MakeError(FString::Printf(TEXT("Could not load base UMaterial: %s"), *MaterialPath));

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: Recompile Material")));
		Mat->Modify();

		if (bLayout)
		{
			UMaterialEditingLibrary::LayoutMaterialExpressions(Mat);
		}
		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("material_path"), MaterialPath);
		Result->SetBoolField(TEXT("layout_expressions"), bLayout);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: material/recompile %s (layout=%s)"),
			*MaterialPath, bLayout ? TEXT("true") : TEXT("false"));
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
			 "Warning: an unknown parameter_name now returns an error (the handler verifies the param exists on the instance/parent before setting); this only marks the package dirty (MarkPackageDirty), it does NOT save to disk."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path, e.g. /Game/MI_Foo.MI_Foo"))
		.RequiredString(TEXT("parameter_name"), TEXT("Scalar parameter name as defined in the parent material"))
		.RequiredNumber(TEXT("value"), TEXT("New scalar value (unitless float)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_vector_parameter"),
		TEXT("Override a vector parameter on a UMaterialInstanceConstant asset with a linear color (editor-only, cheap, no recompile). "
			 "Returns {instance_path, parameter_name, value}. Value is a FLinearColor in linear space (channels 0..1, may exceed 1 for HDR) - NOT a position in cm and NOT degrees. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, required), value (array [R,G,B] or [R,G,B,A], required). "
			 "Workflow: list_parameters to discover vector names; pair with set_scalar/set_texture for a consistent look. "
			 "Warning: alpha defaults to 1.0 when omitted; an unknown parameter_name now returns an error (existence verified before setting); only marks the package dirty - save the asset to persist."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Vector parameter name as defined in the parent material"))
		.RequiredColor(TEXT("value"), TEXT("Linear color [R,G,B] or [R,G,B,A], channels 0..1 (HDR may exceed 1)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_texture_parameter"),
		TEXT("Override a texture parameter on a UMaterialInstanceConstant asset (editor-only set). "
			 "Returns {instance_path, parameter_name, texture_path}. The handler loads the texture object internally, so you pass a content path string, not a loaded object. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, required), texture_path (string, UTexture object path Pkg.Asset, required). "
			 "Workflow: use assets/find to locate textures -> set_texture_parameter -> save the asset to persist. "
			 "Warning: texture_path must load (fails otherwise); an unknown parameter_name now returns an error (existence verified before setting); only marks the package dirty, does not save; texture compression / shader work can make the swap non-instant."))
		.RequiredString(TEXT("instance_path"), TEXT("UMaterialInstanceConstant object path"))
		.RequiredString(TEXT("parameter_name"), TEXT("Texture parameter name as defined in the parent material"))
		.RequiredString(TEXT("texture_path"), TEXT("UTexture object path, e.g. /Game/Tex/T_Grass.T_Grass"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_static_switch"),
		TEXT("Override a static switch on a UMaterialInstanceConstant and rebuild its static permutation. "
			 "Returns {instance_path, parameter_name, value}. If the switch isn't already overridden the handler adds the override entry. "
			 "Params: instance_path (string, MIC object path, required), parameter_name (string, required), value (boolean, required). "
			 "Workflow: introspect static switch names in the parent Material Editor first; batch multiple switch sets, then save once. "
			 "Warning: an unknown parameter_name now returns an error (existence verified via GetStaticSwitchParameterValue before setting); changing a static switch forces a FULL shader-permutation recompile (UpdateStaticPermutation) - expensive, can take several seconds, so avoid looping; re-applying the same value still rebuilds; only marks the package dirty, does not save to disk."))
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

	Tools.Add(FMCPToolBuilder(TEXT("add_expression"),
		TEXT("Add a material-graph expression node to a base UMaterial (NOT a UMaterialInstanceConstant) via UMaterialEditingLibrary::CreateMaterialExpression. "
			 "Returns {material_path, expression_name, expression_class}; expression_name is the engine-assigned object name you pass to connect_expression (handles do not persist across MCP calls). "
			 "Params: material_path (string, base UMaterial object path, required), expression_class (string, one of MaterialExpressionTextureSample/ScalarParameter/VectorParameter/Constant/Multiply/Add/TextureCoordinate, required), node_pos_x (integer, graph X, optional), node_pos_y (integer, graph Y, optional). "
			 "Workflow: add_expression for each node -> connect_expression to wire outputs to inputs/material properties -> recompile once at the end. "
			 "Warning: an unknown expression_class returns an error; this does NOT recompile shaders per call (deliberately cheap) and only marks the package dirty, so changes are NOT reflected until you call recompile; only base UMaterial assets are supported, not instances."))
		.RequiredString(TEXT("material_path"), TEXT("Base UMaterial object path, e.g. /Game/Materials/M_Base.M_Base"))
		.RequiredString(TEXT("expression_class"), TEXT("Expression class short name, e.g. MaterialExpressionScalarParameter"))
		.OptionalInteger(TEXT("node_pos_x"), TEXT("X position of the new node in the material graph (default 0)"))
		.OptionalInteger(TEXT("node_pos_y"), TEXT("Y position of the new node in the material graph (default 0)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("connect_expression"),
		TEXT("Wire one material expression's output to another expression's input, OR to a base material property, on a base UMaterial. Expressions are re-found by name each call since handles do not persist. "
			 "Returns {material_path, from_expression, (to_expression|to_property), connected}. "
			 "Params: material_path (string, base UMaterial object path, required), from_expression (string, source node name from add_expression, required), from_output (string, source output pin name, empty = first output, optional), to_expression (string, target node name) OR to_property (string, material input), to_input (string, target input pin name, empty = first input, optional). "
			 "Workflow: add_expression to create nodes -> connect_expression to wire them and connect the final node to a material property -> recompile. "
			 "Warning: provide EXACTLY one of to_expression or to_property; valid to_property values are BaseColor/Metallic/Specular/Roughness/Normal/EmissiveColor/Opacity/OpacityMask/AmbientOcclusion (WorldPositionOffset is hidden and rejected); missing from/to expressions error; the underlying bool result is surfaced as 'connected'; does NOT recompile - call recompile to apply."))
		.RequiredString(TEXT("material_path"), TEXT("Base UMaterial object path"))
		.RequiredString(TEXT("from_expression"), TEXT("Name of the source expression (from add_expression)"))
		.OptionalString(TEXT("from_output"), TEXT("Source output pin name; empty selects the first output"))
		.OptionalString(TEXT("to_expression"), TEXT("Name of the target expression (mutually exclusive with to_property)"))
		.OptionalString(TEXT("to_input"), TEXT("Target input pin name; empty selects the first input"))
		.OptionalString(TEXT("to_property"), TEXT("Material property input: BaseColor/Metallic/Specular/Roughness/Normal/EmissiveColor/Opacity/OpacityMask/AmbientOcclusion"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_base_properties"),
		TEXT("Set top-level rendering properties on a base UMaterial: blend mode, shading model, two-sided, and opacity-mask clip value. All fields are optional; supply only the ones you want to change. "
			 "Returns {material_path, changed}; changed is false when every supplied value already matched (idempotent skip, no PostEditChange). "
			 "Params: material_path (string, base UMaterial object path, required), blend_mode (string Opaque/Masked/Translucent/Additive/Modulate/AlphaComposite/AlphaHoldout, optional), shading_model (string Unlit/DefaultLit/Subsurface/PreintegratedSkin/ClearCoat/SubsurfaceProfile/TwoSidedFoliage/Hair/Cloth/Eye/SingleLayerWater/ThinTranslucent, optional), two_sided (boolean, optional), opacity_mask_clip_value (number, optional). "
			 "Workflow: set blend_mode=Masked before wiring an OpacityMask node, then set opacity_mask_clip_value; call recompile afterwards to apply shader changes. "
			 "Warning: you must supply at least one optional field or the call errors; unknown enum strings error before the asset is touched; PostEditChange may trigger shader work even though full recompile is deferred to the recompile tool; only marks the package dirty, does not save to disk."))
		.RequiredString(TEXT("material_path"), TEXT("Base UMaterial object path"))
		.OptionalEnum(TEXT("blend_mode"), { TEXT("Opaque"), TEXT("Masked"), TEXT("Translucent"), TEXT("Additive"), TEXT("Modulate"), TEXT("AlphaComposite"), TEXT("AlphaHoldout") }, TEXT("EBlendMode value"))
		.OptionalEnum(TEXT("shading_model"), { TEXT("Unlit"), TEXT("DefaultLit"), TEXT("Subsurface"), TEXT("PreintegratedSkin"), TEXT("ClearCoat"), TEXT("SubsurfaceProfile"), TEXT("TwoSidedFoliage"), TEXT("Hair"), TEXT("Cloth"), TEXT("Eye"), TEXT("SingleLayerWater"), TEXT("ThinTranslucent") }, TEXT("EMaterialShadingModel value"))
		.OptionalBool(TEXT("two_sided"), TEXT("Render both faces (disables backface culling)"))
		.OptionalNumber(TEXT("opacity_mask_clip_value"), TEXT("Clip threshold for BLEND_Masked (0..1)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("recompile"),
		TEXT("Recompile a base UMaterial after graph/property edits via UMaterialEditingLibrary::RecompileMaterial, optionally laying out expression nodes first. This is what makes add_expression/connect_expression/set_base_properties changes actually take effect. "
			 "Returns {material_path, layout_expressions}. "
			 "Params: material_path (string, base UMaterial object path, required), layout_expressions (boolean, run LayoutMaterialExpressions to tidy the graph grid before compiling, optional, default false). "
			 "Workflow: do all add_expression/connect_expression/set_base_properties edits first, then call recompile ONCE; save the asset afterwards to persist. "
			 "Warning: this is an EXPENSIVE blocking shader compile that runs on the game thread and can take several seconds (longer for many permutations) - call it once after batching edits, never in a loop; it only marks the package dirty, it does NOT save to disk."))
		.RequiredString(TEXT("material_path"), TEXT("Base UMaterial object path"))
		.OptionalBool(TEXT("layout_expressions"), TEXT("Tidy the node graph with LayoutMaterialExpressions before compiling"))
		.Build());

	return Tools;
}

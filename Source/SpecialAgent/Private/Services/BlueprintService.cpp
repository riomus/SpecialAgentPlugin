// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/BlueprintService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

namespace
{
	// Resolve a type string (e.g., "bool", "int", "float", "string", "name", "vector",
	// "rotator", "transform") to an FEdGraphPinType for blueprint variables.
	// Returns false if the type is unknown.
	static bool ResolvePinType(const FString& TypeIn, FEdGraphPinType& OutPinType)
	{
		const FString Type = TypeIn.ToLower();
		OutPinType = FEdGraphPinType();
		OutPinType.ContainerType = EPinContainerType::None;

		if (Type == TEXT("bool") || Type == TEXT("boolean"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
		if (Type == TEXT("int") || Type == TEXT("int32") || Type == TEXT("integer"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (Type == TEXT("int64"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			return true;
		}
		if (Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("real"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (Type == TEXT("string") || Type == TEXT("fstring"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
			return true;
		}
		if (Type == TEXT("name") || Type == TEXT("fname"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		}
		if (Type == TEXT("text") || Type == TEXT("ftext"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			return true;
		}
		if (Type == TEXT("vector") || Type == TEXT("fvector"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			return true;
		}
		if (Type == TEXT("rotator") || Type == TEXT("frotator"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			return true;
		}
		if (Type == TEXT("transform") || Type == TEXT("ftransform"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			return true;
		}
		if (Type == TEXT("color") || Type == TEXT("flinearcolor"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
			return true;
		}
		return false;
	}

	// Stringify an FEdGraphPinType for list_variables output.
	static FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject.IsValid())
		{
			return FString::Printf(TEXT("struct:%s"), *PinType.PinSubCategoryObject->GetName());
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object && PinType.PinSubCategoryObject.IsValid())
		{
			return FString::Printf(TEXT("object:%s"), *PinType.PinSubCategoryObject->GetName());
		}
		return PinType.PinCategory.ToString();
	}

	// Find a UClass by short name or fully-qualified path. Null on failure.
	static UClass* ResolveClass(const FString& ClassName)
	{
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}
		// Try path-style first (e.g., /Script/Engine.Actor).
		if (UClass* ByPath = FindFirstObjectSafe<UClass>(*ClassName))
		{
			return ByPath;
		}
		// Fall back to iterating by short name.
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ClassName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// Load a UBlueprint at an asset path. Returns null if the path does not
	// resolve to a Blueprint asset.
	static UBlueprint* LoadBlueprintAtPath(const FString& AssetPath)
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		return Cast<UBlueprint>(Asset);
	}

	// Save a package to disk at its default asset filename.
	static bool SavePackageToDisk(UPackage* Package)
	{
		if (!Package) return false;
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, nullptr, *FileName, Args);
	}
}

FString FBlueprintService::GetServiceDescription() const
{
	return TEXT("Blueprint asset creation, compilation, and reflection");
}

FMCPResponse FBlueprintService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("create"))            return HandleCreate(Request);
	if (MethodName == TEXT("compile"))           return HandleCompile(Request);
	if (MethodName == TEXT("add_variable"))      return HandleAddVariable(Request);
	if (MethodName == TEXT("add_function"))      return HandleAddFunction(Request);
	if (MethodName == TEXT("set_default_value")) return HandleSetDefaultValue(Request);
	if (MethodName == TEXT("list_functions"))    return HandleListFunctions(Request);
	if (MethodName == TEXT("list_variables"))    return HandleListVariables(Request);
	if (MethodName == TEXT("open_in_editor"))    return HandleOpenInEditor(Request);
	if (MethodName == TEXT("duplicate"))         return HandleDuplicate(Request);
	if (MethodName == TEXT("reparent"))          return HandleReparent(Request);

	return MethodNotFound(Request.Id, TEXT("blueprint"), MethodName);
}

FMCPResponse FBlueprintService::HandleCreate(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath, ParentClassName;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parent_class'"));
	}

	auto Task = [AssetPath, ParentClassName]() -> TSharedPtr<FJsonObject>
	{
		UClass* ParentClass = ResolveClass(ParentClassName);
		if (!ParentClass)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: blueprint/create — parent class not found: %s"), *ParentClassName);
			return FMCPJson::MakeError(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName));
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		const FString AssetName = FPackageName::ObjectPathToObjectName(AssetPath);
		if (PackageName.IsEmpty() || AssetName.IsEmpty())
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
		}
		Package->FullyLoad();

		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());

		if (!Blueprint)
		{
			return FMCPJson::MakeError(TEXT("CreateBlueprint returned null"));
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Blueprint->MarkPackageDirty();
		SavePackageToDisk(Package);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("name"), Blueprint->GetName());
		Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/create — created %s (parent %s)"),
			*Blueprint->GetPathName(), *ParentClass->GetName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleCompile(const FMCPRequest& Request)
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

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: blueprint/compile — blueprint not found: %s"), *AssetPath);
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Result->SetNumberField(TEXT("status"), (int32)Blueprint->Status);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/compile — compiled %s status=%d"),
			*Blueprint->GetPathName(), (int32)Blueprint->Status);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleAddVariable(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath, VarName, VarType;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VarName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_type"), VarType))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_type'"));
	}

	auto Task = [AssetPath, VarName, VarType]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		FEdGraphPinType PinType;
		if (!ResolvePinType(VarType, PinType))
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Unknown variable_type: %s"), *VarType));
		}

		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType);
		if (!bAdded)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("AddMemberVariable failed for '%s' (duplicate name?)"), *VarName));
		}

		Blueprint->MarkPackageDirty();
		SavePackageToDisk(Blueprint->GetOutermost());

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("variable_name"), VarName);
		Result->SetStringField(TEXT("variable_type"), PinTypeToString(PinType));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/add_variable — %s.%s (%s)"),
			*Blueprint->GetName(), *VarName, *PinTypeToString(PinType));
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleAddFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath, FunctionName;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}

	auto Task = [AssetPath, FunctionName]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FName(*FunctionName),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!NewGraph)
		{
			return FMCPJson::MakeError(TEXT("CreateNewGraph returned null"));
		}

		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated*/ true, /*SignatureFromClass*/ nullptr);
		Blueprint->MarkPackageDirty();
		SavePackageToDisk(Blueprint->GetOutermost());

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("function_name"), FunctionName);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/add_function — %s.%s"),
			*Blueprint->GetName(), *FunctionName);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleSetDefaultValue(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath, PropertyName, Value;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'property_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("value"), Value))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'value'"));
	}

	auto Task = [AssetPath, PropertyName, Value]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint || !Blueprint->GeneratedClass)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint or GeneratedClass not found: %s"), *AssetPath));
		}

		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		if (!CDO)
		{
			return FMCPJson::MakeError(TEXT("Blueprint CDO is null"));
		}

		FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, FName(*PropertyName));
		if (!Property)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Property not found on CDO: %s"), *PropertyName));
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
		const TCHAR* ImportResult = Property->ImportText_Direct(*Value, ValuePtr, CDO, PPF_None);
		if (ImportResult == nullptr)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("ImportText_Direct failed for '%s' with value '%s'"), *PropertyName, *Value));
		}

		Blueprint->MarkPackageDirty();
		SavePackageToDisk(Blueprint->GetOutermost());

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("property_name"), PropertyName);
		Result->SetStringField(TEXT("value"), Value);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/set_default_value — %s.%s = %s"),
			*Blueprint->GetName(), *PropertyName, *Value);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleListFunctions(const FMCPRequest& Request)
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

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		TArray<TSharedPtr<FJsonValue>> FunctionsArr;

		// User-created function graphs.
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!Graph) continue;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Graph->GetName());
			Obj->SetStringField(TEXT("kind"), TEXT("graph"));
			FunctionsArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		// Native + generated class functions (signature view).
		if (Blueprint->GeneratedClass)
		{
			for (TFieldIterator<UFunction> It(Blueprint->GeneratedClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Function = *It;
				if (!Function) continue;
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), Function->GetName());
				Obj->SetStringField(TEXT("kind"), TEXT("ufunction"));
				Obj->SetNumberField(TEXT("num_params"), Function->NumParms);
				FunctionsArr.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("functions"), FunctionsArr);
		Result->SetNumberField(TEXT("count"), FunctionsArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/list_functions — %s: %d"),
			*Blueprint->GetName(), FunctionsArr.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleListVariables(const FMCPRequest& Request)
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

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		TArray<TSharedPtr<FJsonValue>> VarsArr;
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
			Obj->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
			VarsArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("variables"), VarsArr);
		Result->SetNumberField(TEXT("count"), VarsArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/list_variables — %s: %d"),
			*Blueprint->GetName(), VarsArr.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleOpenInEditor(const FMCPRequest& Request)
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

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditor)
			{
				return FMCPJson::MakeError(TEXT("UAssetEditorSubsystem unavailable"));
			}
			const bool bOpened = AssetEditor->OpenEditorForAsset(Blueprint);
			if (!bOpened)
			{
				return FMCPJson::MakeError(TEXT("OpenEditorForAsset returned false"));
			}
		}
		else
		{
			return FMCPJson::MakeError(TEXT("GEditor unavailable"));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/open_in_editor — %s"), *Blueprint->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleDuplicate(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath, NewName, NewPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_path"), NewPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_path'"));
	}

	auto Task = [AssetPath, NewName, NewPath]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* SrcBlueprint = LoadBlueprintAtPath(AssetPath);
		if (!SrcBlueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Duplicated = AssetTools.DuplicateAsset(NewName, NewPath, SrcBlueprint);
		if (!Duplicated)
		{
			return FMCPJson::MakeError(TEXT("DuplicateAsset returned null"));
		}

		Duplicated->MarkPackageDirty();
		SavePackageToDisk(Duplicated->GetOutermost());

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("source_path"), SrcBlueprint->GetPathName());
		Result->SetStringField(TEXT("new_path"), Duplicated->GetPathName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/duplicate — %s -> %s"),
			*SrcBlueprint->GetPathName(), *Duplicated->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FBlueprintService::HandleReparent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath, NewParentName;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_parent_class"), NewParentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_parent_class'"));
	}

	auto Task = [AssetPath, NewParentName]() -> TSharedPtr<FJsonObject>
	{
		UBlueprint* Blueprint = LoadBlueprintAtPath(AssetPath);
		if (!Blueprint)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		UClass* NewParent = ResolveClass(NewParentName);
		if (!NewParent)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Parent class not found: %s"), *NewParentName));
		}

		Blueprint->ParentClass = NewParent;
		FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		Blueprint->MarkPackageDirty();
		SavePackageToDisk(Blueprint->GetOutermost());

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("new_parent_class"), NewParent->GetName());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: blueprint/reparent — %s -> %s"),
			*Blueprint->GetPathName(), *NewParent->GetName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

TArray<FMCPToolInfo> FBlueprintService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(TEXT("create"),
		TEXT("Create a new Blueprint asset. Persists the asset to disk under the given content path.\n"
			"Params: asset_path (string, /Game/..., content-path+name), parent_class (string, UClass name or path, e.g., Actor, /Script/Engine.Actor).\n"
			"Workflow: create -> add_variable/add_function -> compile -> open_in_editor.\n"
			"Warning: Overwrites any existing asset at asset_path."))
		.RequiredString(TEXT("asset_path"), TEXT("Target Blueprint asset path, e.g., /Game/BP/MyBlueprint.MyBlueprint"))
		.RequiredString(TEXT("parent_class"), TEXT("Parent UClass (short name like 'Actor' or path like /Script/Engine.Actor)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("compile"),
		TEXT("Compile a Blueprint. Produces a fresh UBlueprintGeneratedClass and refreshes the CDO.\n"
			"Params: asset_path (string, /Game/..., blueprint to compile).\n"
			"Workflow: add_variable/add_function -> compile -> (run in PIE).\n"
			"Warning: Fails silently with non-zero Blueprint->Status on error — inspect the returned status."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("add_variable"),
		TEXT("Add a member variable to a Blueprint's NewVariables list.\n"
			"Params: asset_path (string, /Game/..., target blueprint), variable_name (string, FName-safe), "
			"variable_type (string, one of: bool, int, int64, float, string, name, text, vector, rotator, transform, color).\n"
			"Workflow: add_variable -> compile -> set_default_value.\n"
			"Warning: Fails if variable_name already exists on the Blueprint."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.RequiredString(TEXT("variable_name"), TEXT("Variable name (FName)"))
		.RequiredString(TEXT("variable_type"), TEXT("Type token: bool|int|int64|float|string|name|text|vector|rotator|transform|color"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("add_function"),
		TEXT("Add a user function graph to a Blueprint.\n"
			"Params: asset_path (string, /Game/..., target blueprint), function_name (string, FName-safe).\n"
			"Workflow: add_function -> open_in_editor (wire nodes in the graph) -> compile.\n"
			"Warning: Creates an empty graph; caller must wire nodes separately (UI or Python)."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.RequiredString(TEXT("function_name"), TEXT("Function graph name (FName)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_default_value"),
		TEXT("Set a default value on the Blueprint CDO via FProperty::ImportText_Direct.\n"
			"Params: asset_path (string, /Game/..., target blueprint), property_name (string, FName on the generated class), "
			"value (string, ImportText-compatible literal, e.g., '42', 'True', '(X=1,Y=2,Z=3)').\n"
			"Workflow: compile -> set_default_value -> compile (to propagate to instances).\n"
			"Warning: Value string must match UE ImportText syntax for the property's FProperty type."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.RequiredString(TEXT("property_name"), TEXT("Property name on the generated class"))
		.RequiredString(TEXT("value"), TEXT("ImportText-compatible string literal"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_functions"),
		TEXT("List function graphs and UFunctions exposed by a Blueprint's generated class.\n"
			"Params: asset_path (string, /Game/..., target blueprint).\n"
			"Workflow: list_functions -> call_function (reflection service) or open_in_editor.\n"
			"Warning: Includes inherited UFunctions; filter on 'kind' to distinguish user graphs."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_variables"),
		TEXT("List member variables declared on a Blueprint (NewVariables).\n"
			"Params: asset_path (string, /Game/..., target blueprint).\n"
			"Workflow: list_variables -> set_default_value.\n"
			"Warning: Does not include inherited parent-class variables."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("open_in_editor"),
		TEXT("Open a Blueprint in the editor via UAssetEditorSubsystem::OpenEditorForAsset.\n"
			"Params: asset_path (string, /Game/..., target blueprint).\n"
			"Workflow: Used for manual follow-up after create/add_function.\n"
			"Warning: Requires editor in interactive mode; no-op in -nullrhi/commandlet contexts."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("duplicate"),
		TEXT("Duplicate a Blueprint asset via IAssetTools::DuplicateAsset.\n"
			"Params: asset_path (string, /Game/..., source blueprint), new_name (string, new asset short name), "
			"new_path (string, /Game/..., destination content folder).\n"
			"Workflow: duplicate -> reparent (optional) -> set_default_value.\n"
			"Warning: Fails if new_path/new_name resolves to an existing asset."))
		.RequiredString(TEXT("asset_path"), TEXT("Source Blueprint asset path"))
		.RequiredString(TEXT("new_name"), TEXT("Destination asset name (without path)"))
		.RequiredString(TEXT("new_path"), TEXT("Destination content folder, e.g., /Game/BP"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("reparent"),
		TEXT("Reparent a Blueprint to a new UClass and recompile.\n"
			"Params: asset_path (string, /Game/..., target blueprint), new_parent_class (string, UClass short name or path).\n"
			"Workflow: reparent -> list_variables (check inherited props) -> set_default_value.\n"
			"Warning: Can invalidate nodes referencing removed parent API; review Compile log after."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint asset path"))
		.RequiredString(TEXT("new_parent_class"), TEXT("Destination parent UClass (short name or path)"))
		.Build());

	return Tools;
}

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
		TEXT("Create a new Blueprint asset and save it to disk. Returns {asset_path (object path), name, parent_class}. "
			"Params: asset_path (string, required, virtual object path /Game/Folder/Name.Name, never an OS path), "
			"parent_class (string, required, UClass short name like 'Actor' or full path /Script/Engine.Actor). "
			"Workflow: create -> add_variable/add_function -> compile -> open_in_editor; guard re-runs with blueprint/list_variables or an existence check first. "
			"Warning: persists immediately (writes the .uasset). Re-running with the same asset_path recreates the Blueprint in place, discarding prior graph/variable edits; do not delete+recreate just to re-run."))
		.RequiredString(TEXT("asset_path"), TEXT("Target Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.RequiredString(TEXT("parent_class"), TEXT("Parent UClass: short name like 'Actor' or full path /Script/Engine.Actor"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("compile"),
		TEXT("Compile a Blueprint, rebuilding its generated class and refreshing the CDO. Returns {asset_path, status (int enum: 0=Unknown,1=Dirty,2=Error,3=UpToDate,4=BeingCreated)}. "
			"Params: asset_path (string, required, virtual object path /Game/...). "
			"Workflow: required after structural edits (add_variable/add_function/reparent) before the changes take effect at runtime. "
			"Warning: never raises on a compile error -- it returns a non-zero status (2=Error) instead, so always inspect the returned status. Saving a Blueprint also recompiles, so do not double-compile then save in a tight loop."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("add_variable"),
		TEXT("Add a member variable to a Blueprint and save the asset. Returns {variable_name, variable_type (normalized form, e.g. 'struct:Vector')}. "
			"Params: asset_path (string, required, virtual object path /Game/...), variable_name (string, required, FName-safe), "
			"variable_type (string, required, one of: bool, int, int64, float, string, name, text, vector, rotator, transform, color; aliases like 'boolean', 'integer', 'double', 'fvector' accepted). Struct types map to FVector (cm), FRotator (degrees), FTransform, FLinearColor. "
			"Workflow: add_variable -> compile -> set_default_value (set the CDO default). "
			"Warning: returns an error if variable_name already exists on the Blueprint (duplicate). Persists immediately; the var only takes effect after compile."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.RequiredString(TEXT("variable_name"), TEXT("Variable name (FName-safe)"))
		.RequiredString(TEXT("variable_type"), TEXT("Type token: bool|int|int64|float|string|name|text|vector|rotator|transform|color"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("add_function"),
		TEXT("Add an empty user function graph to a Blueprint and save the asset. Returns {function_name}. "
			"Params: asset_path (string, required, virtual object path /Game/...), function_name (string, required, FName-safe graph name). "
			"Workflow: add_function -> open_in_editor (wire nodes manually in the graph) -> compile. "
			"Warning: only creates the empty graph -- it does not add nodes, so the function does nothing until wired up separately (UI or Python). Persists immediately; effective only after compile."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.RequiredString(TEXT("function_name"), TEXT("Function graph name (FName-safe)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_default_value"),
		TEXT("Set a class-default (CDO) property value on a Blueprint and save the asset. Returns {property_name, value}. "
			"Params: asset_path (string, required, virtual object path /Game/...), property_name (string, required, snake_case FName as it appears on the generated class), "
			"value (string, required, UE ImportText literal: '42', 'true', '(X=1,Y=2,Z=3)' for FVector in cm, '(Pitch=0,Yaw=90,Roll=0)' for FRotator in degrees, '(R=1,G=0,B=0,A=1)' for FLinearColor). "
			"Workflow: compile first so the property exists on the generated class, then set_default_value. "
			"Warning: writes the CDO directly and does NOT recompile; new instances pick up the value but existing placed actors may not refresh. Property must already be reflected on the generated class -- BP-added components live on the construction script, not the CDO, so their sub-properties are not reachable here."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.RequiredString(TEXT("property_name"), TEXT("Property name on the generated class (snake_case FName)"))
		.RequiredString(TEXT("value"), TEXT("UE ImportText literal matching the property type"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_functions"),
		TEXT("List a Blueprint's user function graphs plus the UFunctions on its generated class. Returns {functions[{name, kind ('graph' for user graphs, 'ufunction' for reflected functions), num_params (ufunctions only)}], count}. "
			"Params: asset_path (string, required, virtual object path /Game/...). Read-only, no side effects. "
			"Workflow: list_functions -> open_in_editor to author, or feed a name to a reflection call. "
			"Warning: the 'ufunction' entries include inherited parent/native functions (uses IncludeSuper), so filter on kind=='graph' to see only functions defined on this Blueprint."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_variables"),
		TEXT("List the member variables declared directly on a Blueprint. Returns {variables[{name, type (normalized, e.g. 'bool', 'struct:Vector', 'object:StaticMesh')}], count}. "
			"Params: asset_path (string, required, virtual object path /Game/...). Read-only, no side effects. "
			"Workflow: list_variables -> set_default_value to set a CDO default. "
			"Warning: lists only variables added on this Blueprint (NewVariables); inherited parent-class properties are not included even though they are settable via set_default_value."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("open_in_editor"),
		TEXT("Open a Blueprint in its asset editor window (UAssetEditorSubsystem). Returns {asset_path}. "
			"Params: asset_path (string, required, virtual object path /Game/...). "
			"Workflow: convenience for handing a Blueprint off to a human after create/add_function/add_variable so they can wire graphs by hand. "
			"Warning: requires a running interactive editor with GEditor available; returns an error in -nullrhi / commandlet / headless contexts. Opens a UI window -- no value to a fully automated chain."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("duplicate"),
		TEXT("Duplicate a Blueprint asset to a new location and save the copy. Returns {source_path, new_path (object path of the copy)}. "
			"Params: asset_path (string, required, source Blueprint object path /Game/...), new_name (string, required, bare asset name with no path or extension), "
			"new_path (string, required, destination content FOLDER /Game/... -- a package path, not an object path). "
			"Workflow: duplicate -> reparent (optional) -> set_default_value to tweak the copy. "
			"Warning: persists immediately. Returns an error if new_path + new_name already resolves to an existing asset, so check existence first rather than overwriting."))
		.RequiredString(TEXT("asset_path"), TEXT("Source Blueprint object path, e.g. /Game/BP/Src.Src"))
		.RequiredString(TEXT("new_name"), TEXT("Destination asset name, bare (no path, no extension)"))
		.RequiredString(TEXT("new_path"), TEXT("Destination content folder (package path), e.g. /Game/BP"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("reparent"),
		TEXT("Reparent a Blueprint to a new base UClass; this tool refreshes nodes, recompiles, and saves for you. Returns {asset_path, new_parent_class}. "
			"Params: asset_path (string, required, virtual object path /Game/...), new_parent_class (string, required, UClass short name like 'Pawn' or full path /Script/Engine.Pawn). "
			"Workflow: reparent -> list_variables (check newly inherited props) -> set_default_value. No separate compile call needed -- reparent compiles internally. "
			"Warning: switching parents can invalidate graph nodes that referenced removed parent API; the compile will flag them, so inspect the Output Log afterward. Reparenting to the same class still triggers a full recompile, so skip the call if the parent is unchanged."))
		.RequiredString(TEXT("asset_path"), TEXT("Blueprint object path, e.g. /Game/BP/MyBlueprint.MyBlueprint"))
		.RequiredString(TEXT("new_parent_class"), TEXT("Destination parent UClass: short name or full /Script/... path"))
		.Build());

	return Tools;
}

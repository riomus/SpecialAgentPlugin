// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ReflectionService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPJson.h"
#include "MCPCommon/MCPRequestContext.h"

#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace
{
	// Find a UClass by short name or fully-qualified path. Null on failure.
	static UClass* ResolveClass(const FString& ClassName)
	{
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* ByPath = FindFirstObjectSafe<UClass>(*ClassName))
		{
			return ByPath;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ClassName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// Render an FProperty's type as a short readable string.
	static FString PropertyTypeString(FProperty* Property)
	{
		if (!Property) return TEXT("<null>");
		return Property->GetCPPType();
	}

	// Render a UFunction signature string.
	static FString FunctionSignatureString(UFunction* Function)
	{
		if (!Function) return FString();

		FString Params;
		FProperty* Return = nullptr;
		for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			if (It->PropertyFlags & CPF_ReturnParm)
			{
				Return = *It;
				continue;
			}
			if (!Params.IsEmpty())
			{
				Params += TEXT(", ");
			}
			Params += FString::Printf(TEXT("%s %s"), *PropertyTypeString(*It), *It->GetName());
		}

		const FString ReturnTypeStr = Return ? PropertyTypeString(Return) : TEXT("void");
		return FString::Printf(TEXT("%s %s(%s)"), *ReturnTypeStr, *Function->GetName(), *Params);
	}

	// Whitelist: only primitive FProperty types are accepted as call_function args.
	// Returns true when the property is a scalar primitive we can marshal safely.
	static bool IsPrimitiveProperty(FProperty* Property)
	{
		if (!Property) return false;

		if (Property->IsA<FBoolProperty>()) return true;
		if (Property->IsA<FByteProperty>()) return true;
		if (Property->IsA<FIntProperty>()) return true;
		if (Property->IsA<FInt64Property>()) return true;
		if (Property->IsA<FUInt32Property>()) return true;
		if (Property->IsA<FUInt64Property>()) return true;
		if (Property->IsA<FFloatProperty>()) return true;
		if (Property->IsA<FDoubleProperty>()) return true;
		if (Property->IsA<FStrProperty>()) return true;
		if (Property->IsA<FNameProperty>()) return true;

		// Allow FVector only (per spec reviewer) — other structs are non-primitive.
		if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			return StructProp->Struct == TBaseStructure<FVector>::Get();
		}

		return false;
	}
}

FString FReflectionService::GetServiceDescription() const
{
	return TEXT("UObject / UClass / UProperty / UFunction introspection, plus generic reflected-property setting on any loaded object/asset");
}

FMCPResponse FReflectionService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("list_classes"))    return HandleListClasses(Request);
	if (MethodName == TEXT("get_class_info"))  return HandleGetClassInfo(Request);
	if (MethodName == TEXT("list_properties")) return HandleListProperties(Request);
	if (MethodName == TEXT("list_functions"))  return HandleListFunctions(Request);
	if (MethodName == TEXT("call_function"))   return HandleCallFunction(Request);
	if (MethodName == TEXT("set_asset_property")) return HandleSetAssetProperty(Request);

	return MethodNotFound(Request.Id, TEXT("reflection"), MethodName);
}

FMCPResponse FReflectionService::HandleListClasses(const FMCPRequest& Request)
{
	FString Prefix;
	FString BaseClassName;
	int32 MaxResults = 500;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("prefix"), Prefix);
		Request.Params->TryGetStringField(TEXT("base_class"), BaseClassName);
		Request.Params->TryGetNumberField(TEXT("max_results"), MaxResults);
	}

	auto Task = [Prefix, BaseClassName, MaxResults]() -> TSharedPtr<FJsonObject>
	{
		UClass* BaseClass = nullptr;
		if (!BaseClassName.IsEmpty())
		{
			BaseClass = ResolveClass(BaseClassName);
			if (!BaseClass)
			{
				return FMCPJson::MakeError(FString::Printf(TEXT("base_class not found: %s"), *BaseClassName));
			}
		}

		TArray<TSharedPtr<FJsonValue>> ClassesArr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class) continue;

			if (BaseClass && !Class->IsChildOf(BaseClass))
			{
				continue;
			}
			if (!Prefix.IsEmpty() && !Class->GetName().StartsWith(Prefix))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Class->GetName());
			Obj->SetStringField(TEXT("path"), Class->GetPathName());
			Obj->SetStringField(TEXT("super"), Class->GetSuperClass() ? Class->GetSuperClass()->GetName() : TEXT(""));
			ClassesArr.Add(MakeShared<FJsonValueObject>(Obj));

			if (ClassesArr.Num() >= MaxResults) break;
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("classes"), ClassesArr);
		Result->SetNumberField(TEXT("count"), ClassesArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: reflection/list_classes — %d (prefix='%s', base='%s')"),
			ClassesArr.Num(), *Prefix, *BaseClassName);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FReflectionService::HandleGetClassInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ClassName;
	if (!Request.Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'class_name'"));
	}

	auto Task = [ClassName]() -> TSharedPtr<FJsonObject>
	{
		UClass* Class = ResolveClass(ClassName);
		if (!Class)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Class not found: %s"), *ClassName));
		}

		int32 PropertyCount = 0;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			++PropertyCount;
		}
		int32 FunctionCount = 0;
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			++FunctionCount;
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("name"), Class->GetName());
		Result->SetStringField(TEXT("path"), Class->GetPathName());
		Result->SetStringField(TEXT("super"), Class->GetSuperClass() ? Class->GetSuperClass()->GetName() : TEXT(""));
		Result->SetStringField(TEXT("package"), Class->GetPackage() ? Class->GetPackage()->GetName() : TEXT(""));
		Result->SetNumberField(TEXT("property_count"), PropertyCount);
		Result->SetNumberField(TEXT("function_count"), FunctionCount);
		Result->SetBoolField(TEXT("is_native"), Class->HasAnyClassFlags(CLASS_Native));
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: reflection/get_class_info — %s (props=%d, funcs=%d)"),
			*Class->GetName(), PropertyCount, FunctionCount);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FReflectionService::HandleListProperties(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ClassName;
	if (!Request.Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'class_name'"));
	}

	bool bIncludeInherited = true;
	Request.Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	auto Task = [ClassName, bIncludeInherited]() -> TSharedPtr<FJsonObject>
	{
		UClass* Class = ResolveClass(ClassName);
		if (!Class)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Class not found: %s"), *ClassName));
		}

		TArray<TSharedPtr<FJsonValue>> PropsArr;
		const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
			? EFieldIteratorFlags::IncludeSuper
			: EFieldIteratorFlags::ExcludeSuper;
		for (TFieldIterator<FProperty> It(Class, SuperFlag); It; ++It)
		{
			FProperty* Property = *It;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Property->GetName());
			Obj->SetStringField(TEXT("type"), PropertyTypeString(Property));
			Obj->SetStringField(TEXT("owner"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetName() : TEXT(""));
			PropsArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("properties"), PropsArr);
		Result->SetNumberField(TEXT("count"), PropsArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: reflection/list_properties — %s: %d"),
			*Class->GetName(), PropsArr.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FReflectionService::HandleListFunctions(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ClassName;
	if (!Request.Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'class_name'"));
	}

	bool bIncludeInherited = true;
	Request.Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	auto Task = [ClassName, bIncludeInherited]() -> TSharedPtr<FJsonObject>
	{
		UClass* Class = ResolveClass(ClassName);
		if (!Class)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Class not found: %s"), *ClassName));
		}

		TArray<TSharedPtr<FJsonValue>> FuncsArr;
		const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
			? EFieldIteratorFlags::IncludeSuper
			: EFieldIteratorFlags::ExcludeSuper;
		for (TFieldIterator<UFunction> It(Class, SuperFlag); It; ++It)
		{
			UFunction* Function = *It;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Function->GetName());
			Obj->SetStringField(TEXT("signature"), FunctionSignatureString(Function));
			Obj->SetNumberField(TEXT("num_params"), Function->NumParms);
			Obj->SetStringField(TEXT("owner"), Function->GetOuterUClass() ? Function->GetOuterUClass()->GetName() : TEXT(""));
			FuncsArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetArrayField(TEXT("functions"), FuncsArr);
		Result->SetNumberField(TEXT("count"), FuncsArr.Num());
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: reflection/list_functions — %s: %d"),
			*Class->GetName(), FuncsArr.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FReflectionService::HandleCallFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ObjectPath, FunctionName;
	if (!Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'object_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}

	// Collect string-form args from the "args" JSON array. Primitive only.
	TArray<FString> ArgStrings;
	const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
	if (Request.Params->TryGetArrayField(TEXT("args"), ArgsArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *ArgsArray)
		{
			if (!Value.IsValid())
			{
				ArgStrings.Add(FString());
				continue;
			}
			// Accept scalars as their stringified form; arrays are treated as vectors.
			switch (Value->Type)
			{
				case EJson::String:
					ArgStrings.Add(Value->AsString());
					break;
				case EJson::Number:
					ArgStrings.Add(LexToString(Value->AsNumber()));
					break;
				case EJson::Boolean:
					ArgStrings.Add(Value->AsBool() ? TEXT("true") : TEXT("false"));
					break;
				case EJson::Array:
				{
					// Marshal FVector-sized arrays as UE ImportText syntax (X=..,Y=..,Z=..).
					const TArray<TSharedPtr<FJsonValue>>& InnerArr = Value->AsArray();
					if (InnerArr.Num() == 3)
					{
						ArgStrings.Add(FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"),
							InnerArr[0]->AsNumber(), InnerArr[1]->AsNumber(), InnerArr[2]->AsNumber()));
					}
					else
					{
						return InvalidParams(Request.Id, TEXT("Array args must be 3-element vectors"));
					}
					break;
				}
				default:
					return InvalidParams(Request.Id, TEXT("Non-primitive arg type (object/null) is not supported"));
			}
		}
	}

	auto Task = [ObjectPath, FunctionName, ArgStrings]() -> TSharedPtr<FJsonObject>
	{
		UObject* Target = FindFirstObjectSafe<UObject>(*ObjectPath);
		if (!Target)
		{
			Target = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (!Target)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Object not found: %s"), *ObjectPath));
		}

		UFunction* Function = Target->FindFunction(FName(*FunctionName));
		if (!Function)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Function '%s' not found on '%s'"),
				*FunctionName, *Target->GetClass()->GetName()));
		}

		// Only allow safe, directly-callable functions. Invoking an arbitrary
		// UFunction (net RPC, latent/async, delegate signature, ubergraph stub)
		// through a synthetic stack frame can hit check()s deep in engine code
		// that abort the whole editor — and ProcessEvent cannot guard against
		// that. Require an explicitly-callable function and reject the unsafe
		// classes outright.
		const EFunctionFlags FFlags = Function->FunctionFlags;
		const bool bCallable = (FFlags & (FUNC_BlueprintCallable | FUNC_Exec)) != 0;
		const bool bUnsafe   = (FFlags & (FUNC_Net | FUNC_NetRequest | FUNC_NetResponse | FUNC_Delegate | FUNC_UbergraphFunction)) != 0;
		if (!bCallable || bUnsafe)
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Function '%s' is not safely callable: requires BlueprintCallable or Exec and must not be a net/delegate/ubergraph function"),
				*FunctionName));
		}
		if (Function->HasMetaData(TEXT("Latent")))
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Function '%s' is latent/async and cannot be invoked synchronously via call_function"), *FunctionName));
		}

		// Enforce primitive-only policy on every Parm.
		TArray<FProperty*> ParamProps;
		FProperty* ReturnProp = nullptr;
		for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			if (It->PropertyFlags & CPF_ReturnParm)
			{
				ReturnProp = *It;
				continue;
			}
			if (!IsPrimitiveProperty(*It))
			{
				return FMCPJson::MakeError(FString::Printf(
					TEXT("Non-primitive parameter '%s' (type '%s') — call_function restricted to bool/int/float/string/name/FVector"),
					*It->GetName(), *PropertyTypeString(*It)));
			}
			ParamProps.Add(*It);
		}
		if (ReturnProp && !IsPrimitiveProperty(ReturnProp))
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Non-primitive return type '%s' — call_function restricted to primitive returns"),
				*PropertyTypeString(ReturnProp)));
		}
		if (ArgStrings.Num() != ParamProps.Num())
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Argument count mismatch: expected %d, got %d"), ParamProps.Num(), ArgStrings.Num()));
		}

		// Allocate a zero-initialized parameter frame on the stack.
		uint8* Frame = (uint8*)FMemory_Alloca(Function->ParmsSize);
		FMemory::Memzero(Frame, Function->ParmsSize);

		// InitializeValue for every Parm so constructors run (FString etc.).
		for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Frame);
		}

		// Import each arg into its parameter slot.
		for (int32 i = 0; i < ParamProps.Num(); ++i)
		{
			FProperty* Prop = ParamProps[i];
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Frame);
			const TCHAR* ImportResult = Prop->ImportText_Direct(*ArgStrings[i], ValuePtr, Target, PPF_None);
			if (ImportResult == nullptr)
			{
				// Destroy partially-initialized frame before returning.
				for (TFieldIterator<FProperty> DIt(Function); DIt && (DIt->PropertyFlags & CPF_Parm); ++DIt)
				{
					DIt->DestroyValue_InContainer(Frame);
				}
				return FMCPJson::MakeError(FString::Printf(
					TEXT("Failed to parse arg[%d]='%s' into %s"), i, *ArgStrings[i], *Prop->GetName()));
			}
		}

		// Invoke.
		Target->ProcessEvent(Function, Frame);

		// Extract return value (if any).
		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("object_path"), Target->GetPathName());
		Result->SetStringField(TEXT("function_name"), FunctionName);

		if (ReturnProp)
		{
			void* RetPtr = ReturnProp->ContainerPtrToValuePtr<void>(Frame);
			FString RetAsText;
			ReturnProp->ExportTextItem_Direct(RetAsText, RetPtr, nullptr, Target, PPF_None);
			Result->SetStringField(TEXT("return_value"), RetAsText);
		}

		// Destroy parameter frame.
		for (TFieldIterator<FProperty> DIt(Function); DIt && (DIt->PropertyFlags & CPF_Parm); ++DIt)
		{
			DIt->DestroyValue_InContainer(Frame);
		}

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: reflection/call_function — %s::%s (args=%d)"),
			*Target->GetClass()->GetName(), *FunctionName, ArgStrings.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FReflectionService::HandleSetAssetProperty(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ObjectPath, PropertyName;
	if (!Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'object_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'property_name'"));
	}

	// 'value' may arrive as a string (UE ImportText form) or as any JSON value
	// (number / bool / array / object) which we stringify into ImportText form.
	FString ValueStr;
	const TSharedPtr<FJsonValue> ValueField = Request.Params->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'value'"));
	}
	switch (ValueField->Type)
	{
		case EJson::String:
			ValueStr = ValueField->AsString();
			break;
		case EJson::Number:
			ValueStr = LexToString(ValueField->AsNumber());
			break;
		case EJson::Boolean:
			ValueStr = ValueField->AsBool() ? TEXT("true") : TEXT("false");
			break;
		case EJson::Array:
		{
			// Marshal a 3-element [X,Y,Z] array as UE ImportText struct syntax.
			const TArray<TSharedPtr<FJsonValue>>& InnerArr = ValueField->AsArray();
			if (InnerArr.Num() == 3)
			{
				ValueStr = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"),
					InnerArr[0]->AsNumber(), InnerArr[1]->AsNumber(), InnerArr[2]->AsNumber());
			}
			else
			{
				return InvalidParams(Request.Id, TEXT("Array 'value' must be a 3-element [X,Y,Z] vector"));
			}
			break;
		}
		default:
			return InvalidParams(Request.Id, TEXT("'value' must be a string, number, bool, or 3-element array"));
	}

	bool bSave = false;
	Request.Params->TryGetBoolField(TEXT("save"), bSave);

	auto Task = [ObjectPath, PropertyName, ValueStr, bSave]() -> TSharedPtr<FJsonObject>
	{
		UObject* Target = FindFirstObjectSafe<UObject>(*ObjectPath);
		if (!Target)
		{
			Target = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (!Target)
		{
			return FMCPJson::MakeError(FString::Printf(TEXT("Object not found: %s"), *ObjectPath));
		}

		FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			// List a few available property names to aid discovery.
			FString Available;
			int32 Shown = 0;
			for (TFieldIterator<FProperty> It(Target->GetClass(), EFieldIteratorFlags::IncludeSuper); It && Shown < 20; ++It, ++Shown)
			{
				if (!Available.IsEmpty()) Available += TEXT(", ");
				Available += It->GetName();
			}
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Property '%s' not found on '%s'. Available (first %d): %s"),
				*PropertyName, *Target->GetClass()->GetName(), Shown, *Available));
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("SpecialAgent: set asset property")));
		Target->Modify();
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
		const TCHAR* ImportResult = Property->ImportText_Direct(*ValueStr, ValuePtr, Target, PPF_None);
		if (ImportResult == nullptr)
		{
			return FMCPJson::MakeError(FString::Printf(
				TEXT("Failed to import value '%s' into property '%s' (type '%s')"),
				*ValueStr, *PropertyName, *PropertyTypeString(Property)));
		}

		Target->PostEditChange();
		Target->MarkPackageDirty();

		// Read back the value as it now stands (post-import canonical form).
		FString NewValue;
		Property->ExportTextItem_Direct(NewValue, ValuePtr, nullptr, Target, PPF_None);

		bool bSaved = false;
		if (bSave)
		{
			UPackage* Package = Target->GetOutermost();
			if (Package)
			{
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Package->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags = SAVE_NoError;
				bSaved = UPackage::SavePackage(Package, nullptr, *FileName, Args);
			}
		}

		TSharedPtr<FJsonObject> Result = FMCPJson::MakeSuccess();
		Result->SetStringField(TEXT("object_path"), Target->GetPathName());
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("new_value"), NewValue);
		Result->SetBoolField(TEXT("saved"), bSaved);
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: reflection/set_asset_property — %s.%s = %s (saved=%d)"),
			*Target->GetClass()->GetName(), *PropertyName, *NewValue, bSaved ? 1 : 0);
		return Result;
	};

	return FMCPResponse::Success(Request.Id,
		FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

TArray<FMCPToolInfo> FReflectionService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(TEXT("list_classes"),
		TEXT("List native and Blueprint UClass objects currently loaded in the editor, optionally filtered by name prefix and/or base class. Returns {classes:[{name, path, super}], count}.\n"
			"Params: prefix (string, optional, case-sensitive StartsWith match on the class name — note class names have no U/A/F prefix here, e.g. 'StaticMesh'); base_class (string, optional, short name or path of a UClass — only its descendants are returned; errors if not found); max_results (integer, optional, default 500, caps the result).\n"
			"Workflow: list_classes -> reflection/get_class_info -> reflection/list_properties / reflection/list_functions.\n"
			"Warning: iterates the entire loaded UClass set; an unfiltered call on a large project is slow and hits the max_results cap silently — always pass a prefix or base_class."))
		.OptionalString(TEXT("prefix"), TEXT("Filter by class-name prefix (case-sensitive)"))
		.OptionalString(TEXT("base_class"), TEXT("Only return descendants of this UClass"))
		.OptionalInteger(TEXT("max_results"), TEXT("Cap on returned classes (default 500)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("get_class_info"),
		TEXT("Describe one UClass. Returns {name, path, super, package, property_count, function_count, is_native}.\n"
			"Params: class_name (string, required, UClass short name like 'Actor' or full path; resolved by exact name match then path lookup).\n"
			"Workflow: get_class_info -> reflection/list_properties / reflection/list_functions for the actual members.\n"
			"Warning: property_count and function_count INCLUDE inherited members; call list_properties/list_functions with include_inherited=false for the own-declared subset. Errors if the class is not loaded."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name or path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_properties"),
		TEXT("List the FProperty fields on a UClass with their C++ type strings. Returns {properties:[{name, type, owner (declaring class)}], count}.\n"
			"Params: class_name (string, required, UClass short name or path); include_inherited (boolean, optional, default true — set false for own-declared properties only).\n"
			"Workflow: list_properties -> reflection/call_function or blueprint/set_default_value using the property name.\n"
			"Warning: 'type' comes from FProperty::GetCPPType() and may differ from the source declaration (e.g. TArray<...> wrappers, enum underlying types). Errors if the class is not loaded."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name or path"))
		.OptionalBool(TEXT("include_inherited"), TEXT("Include parent-class properties (default true)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_functions"),
		TEXT("List the UFunction members on a UClass with reconstructed signatures. Returns {functions:[{name, signature, num_params, owner (declaring class)}], count}.\n"
			"Params: class_name (string, required, UClass short name or path); include_inherited (boolean, optional, default true — set false for own-declared functions only).\n"
			"Workflow: list_functions -> reflection/call_function, but only functions that are BlueprintCallable/Exec and primitive-only are actually invokable there.\n"
			"Warning: this lists ALL UFunctions including non-invokable internals (net RPCs, delegate signatures, ubergraph/latent stubs); call_function will reject those. Errors if the class is not loaded."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name or path"))
		.OptionalBool(TEXT("include_inherited"), TEXT("Include parent-class functions (default true)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("call_function"),
		TEXT("Invoke a UFunction on a live UObject via ProcessEvent with primitive args only (bool, int, float, string, FName, FVector). Returns {object_path, function_name, return_value (text-exported, only if the function returns a primitive)}.\n"
			"Params: object_path (string, required, full UObject path — e.g. /Game/Map.Map:PersistentLevel.MyActor — resolved in-memory then via LoadObject); function_name (string, required, exact UFunction name); args (array, optional, one entry per non-return parameter in declared order — numbers/bools/strings, or a 3-element [X,Y,Z] cm array marshalled to FVector via UE ImportText).\n"
			"Workflow: reflection/list_functions to read the signature -> call_function -> inspect return_value.\n"
			"Warning: only functions flagged BlueprintCallable or Exec are allowed; net (RPC), delegate, ubergraph, and latent/async functions are rejected to avoid editor-aborting check()s. Any struct other than FVector, or object/array/map params, are rejected (use blueprint/set_default_value or python/execute instead). arg count must exactly match the parameter count or it errors."))
		.RequiredString(TEXT("object_path"), TEXT("Full UObject path of the call target"))
		.RequiredString(TEXT("function_name"), TEXT("UFunction name to invoke"))
		.OptionalArrayOfString(TEXT("args"), TEXT("One entry per non-return parameter, in declared order: bool/int/float/string/FName values, or a 3-element [X,Y,Z] array (cm) marshalled to an FVector."))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("set_asset_property"),
		TEXT("Set any reflected UPROPERTY on any loaded UObject (DataAsset, asset config, settings object, in-memory actor) by parsing a text value through the property's own ImportText. Returns {object_path, property, new_value (canonical text read-back), saved}.\n"
			"Params: object_path (string, required, full UObject path — e.g. /Game/Data/MyConfig.MyConfig — resolved in-memory then via LoadObject); property_name (string, required, exact reflected UPROPERTY name — case-sensitive, no type prefix); value (required, a string in UE text/ImportText form, or a JSON number/bool/3-element [X,Y,Z] array which is stringified to that form); save (boolean, optional, default false — when true the owning package is written to disk).\n"
			"Workflow: reflection/list_properties to read the exact property name and type -> set_asset_property with a matching ImportText value -> optionally save=true to persist.\n"
			"Warning: the value must be valid ImportText for that property's type or the import fails and nothing is written (structs use (X=..,Y=..) syntax, object refs use full asset paths, enums use the enumerator name). The change is transacted (undoable) and marks the package dirty; pass save=true only when you intend to overwrite the asset file on disk. If the property name is unknown the error lists the first available property names."))
		.RequiredString(TEXT("object_path"), TEXT("Full UObject path of the target asset/object"))
		.RequiredString(TEXT("property_name"), TEXT("Exact reflected UPROPERTY name to set"))
		.RequiredAny(TEXT("value"), TEXT("Value in UE text/ImportText form, or a JSON number/bool/3-element [X,Y,Z] array"))
		.OptionalBool(TEXT("save"), TEXT("Write the owning package to disk after the edit (default false)"))
		.Build());

	return Tools;
}

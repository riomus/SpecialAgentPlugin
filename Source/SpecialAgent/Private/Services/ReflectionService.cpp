// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ReflectionService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPToolBuilder.h"
#include "MCPCommon/MCPJson.h"

#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"

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
	return TEXT("UObject / UClass / UProperty / UFunction introspection");
}

FMCPResponse FReflectionService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("list_classes"))    return HandleListClasses(Request);
	if (MethodName == TEXT("get_class_info"))  return HandleGetClassInfo(Request);
	if (MethodName == TEXT("list_properties")) return HandleListProperties(Request);
	if (MethodName == TEXT("list_functions"))  return HandleListFunctions(Request);
	if (MethodName == TEXT("call_function"))   return HandleCallFunction(Request);

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

TArray<FMCPToolInfo> FReflectionService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	Tools.Add(FMCPToolBuilder(TEXT("list_classes"),
		TEXT("List UClass objects currently loaded in the editor, optionally filtered by prefix or base class.\n"
			"Params: prefix (string, optional, match at start of class name), base_class (string, optional, UClass name/path — descendants only), "
			"max_results (integer, optional, default 500).\n"
			"Workflow: list_classes -> get_class_info -> list_properties/list_functions.\n"
			"Warning: Iterates the full UObject class set — avoid running without filters on large projects."))
		.OptionalString(TEXT("prefix"), TEXT("Filter by class-name prefix (case-sensitive)"))
		.OptionalString(TEXT("base_class"), TEXT("Only return descendants of this UClass"))
		.OptionalInteger(TEXT("max_results"), TEXT("Cap on returned classes (default 500)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("get_class_info"),
		TEXT("Describe a UClass: name, super class, package, counts of inherited properties and functions.\n"
			"Params: class_name (string, UClass short name or path).\n"
			"Workflow: get_class_info -> list_properties / list_functions for the detail.\n"
			"Warning: Counts include inherited members — call list_properties with include_inherited=false for the own-only subset."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name or path"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_properties"),
		TEXT("List FProperty fields on a UClass with type strings.\n"
			"Params: class_name (string, UClass short name or path), include_inherited (boolean, optional, default true).\n"
			"Workflow: list_properties -> call_function / blueprint/set_default_value.\n"
			"Warning: FProperty::GetCPPType() strings may differ from source declarations (e.g., TArray wrappers)."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name or path"))
		.OptionalBool(TEXT("include_inherited"), TEXT("Include parent-class properties (default true)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("list_functions"),
		TEXT("List UFunction members on a UClass with signature strings.\n"
			"Params: class_name (string, UClass short name or path), include_inherited (boolean, optional, default true).\n"
			"Workflow: list_functions -> call_function (if signature is primitive-only).\n"
			"Warning: Includes non-callable internals (RPCs, delegate signatures) — inspect flags via Unreal docs."))
		.RequiredString(TEXT("class_name"), TEXT("UClass short name or path"))
		.OptionalBool(TEXT("include_inherited"), TEXT("Include parent-class functions (default true)"))
		.Build());

	Tools.Add(FMCPToolBuilder(TEXT("call_function"),
		TEXT("Invoke a UFunction on a live UObject via ProcessEvent. Primitive args only: bool, int, float, string, name, FVector.\n"
			"Params: object_path (string, full UObject path — e.g., /Game/Map.Map:PersistentLevel.MyActor), "
			"function_name (string, UFunction name), args (array of primitives, optional, stringified per UE ImportText rules).\n"
			"Workflow: list_functions -> call_function -> inspect return_value.\n"
			"Warning: Returns error if any parameter is a struct other than FVector, or an object/array/map — use blueprint/set_default_value or Python for complex calls."))
		.RequiredString(TEXT("object_path"), TEXT("Full UObject path of the call target"))
		.RequiredString(TEXT("function_name"), TEXT("UFunction name to invoke"))
		.OptionalArrayOfString(TEXT("args"), TEXT("Array of primitive args (bool/int/float/string/name/[X,Y,Z])"))
		.Build());

	return Tools;
}

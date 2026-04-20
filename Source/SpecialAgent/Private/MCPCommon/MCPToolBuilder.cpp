#include "MCPCommon/MCPToolBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FMCPToolBuilder::FMCPToolBuilder(const FString& Name, const FString& Description)
{
	Tool.Name = Name;
	Tool.Description = Description;
	Tool.Parameters = MakeShared<FJsonObject>();
}

// ---- Scalars ----

FMCPToolBuilder& FMCPToolBuilder::RequiredString(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("string"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::RequiredNumber(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("number"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::RequiredInteger(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("integer"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::RequiredBool(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("boolean"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalString(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("string"), Description, /*bRequired=*/false);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalNumber(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("number"), Description, /*bRequired=*/false);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalInteger(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("integer"), Description, /*bRequired=*/false);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalBool(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("boolean"), Description, /*bRequired=*/false);
	return *this;
}

// ---- Compound ----

FMCPToolBuilder& FMCPToolBuilder::RequiredVec3(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("array"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalVec3(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("array"), Description, /*bRequired=*/false);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::RequiredColor(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("array"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalColor(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("array"), Description, /*bRequired=*/false);
	return *this;
}

// ---- Enumerations ----

FMCPToolBuilder& FMCPToolBuilder::RequiredEnum(const FString& Field, const TArray<FString>& Allowed, const FString& Description)
{
	AddParam(Field, TEXT("string"), Description, /*bRequired=*/true, &Allowed);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalEnum(const FString& Field, const TArray<FString>& Allowed, const FString& Description)
{
	AddParam(Field, TEXT("string"), Description, /*bRequired=*/false, &Allowed);
	return *this;
}

// ---- Array of strings ----

FMCPToolBuilder& FMCPToolBuilder::RequiredArrayOfString(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("array"), Description, /*bRequired=*/true);
	return *this;
}

FMCPToolBuilder& FMCPToolBuilder::OptionalArrayOfString(const FString& Field, const FString& Description)
{
	AddParam(Field, TEXT("array"), Description, /*bRequired=*/false);
	return *this;
}

FMCPToolInfo FMCPToolBuilder::Build() const
{
	return Tool;
}

void FMCPToolBuilder::AddParam(const FString& Field,
                               const FString& JsonType,
                               const FString& Description,
                               bool bRequired,
                               const TArray<FString>* EnumValues)
{
	TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
	ParamObj->SetStringField(TEXT("type"), JsonType);
	ParamObj->SetStringField(TEXT("description"), Description);

	if (EnumValues != nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> EnumArr;
		EnumArr.Reserve(EnumValues->Num());
		for (const FString& Value : *EnumValues)
		{
			EnumArr.Add(MakeShared<FJsonValueString>(Value));
		}
		ParamObj->SetArrayField(TEXT("enum"), EnumArr);
	}

	Tool.Parameters->SetObjectField(Field, ParamObj);
	if (bRequired)
	{
		Tool.RequiredParams.AddUnique(Field);
	}
}

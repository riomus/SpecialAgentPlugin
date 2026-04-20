#include "MCPCommon/MCPJson.h"

#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Math/Color.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"

bool FMCPJson::ReadString(const TSharedPtr<FJsonObject>& Params, const FString& Field, FString& Out)
{
	return Params->TryGetStringField(Field, Out);
}

bool FMCPJson::ReadNumber(const TSharedPtr<FJsonObject>& Params, const FString& Field, double& Out)
{
	return Params->TryGetNumberField(Field, Out);
}

bool FMCPJson::ReadInteger(const TSharedPtr<FJsonObject>& Params, const FString& Field, int32& Out)
{
	double Temp = 0.0;
	if (!Params->TryGetNumberField(Field, Temp))
	{
		return false;
	}
	Out = static_cast<int32>(Temp);
	return true;
}

bool FMCPJson::ReadBool(const TSharedPtr<FJsonObject>& Params, const FString& Field, bool& Out)
{
	return Params->TryGetBoolField(Field, Out);
}

bool FMCPJson::ReadVec3(const TSharedPtr<FJsonObject>& Params, const FString& Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(Field, Arr) || Arr->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	return true;
}

bool FMCPJson::ReadRotator(const TSharedPtr<FJsonObject>& Params, const FString& Field, FRotator& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(Field, Arr) || Arr->Num() != 3)
	{
		return false;
	}
	Out = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	return true;
}

bool FMCPJson::ReadColor(const TSharedPtr<FJsonObject>& Params, const FString& Field, FLinearColor& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(Field, Arr) || Arr->Num() < 3)
	{
		return false;
	}
	const float A = Arr->Num() >= 4 ? static_cast<float>((*Arr)[3]->AsNumber()) : 1.0f;
	Out = FLinearColor(
		static_cast<float>((*Arr)[0]->AsNumber()),
		static_cast<float>((*Arr)[1]->AsNumber()),
		static_cast<float>((*Arr)[2]->AsNumber()),
		A);
	return true;
}

void FMCPJson::WriteVec3(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	Out->SetArrayField(Field, Arr);
}

void FMCPJson::WriteRotator(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FRotator& R)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
	Out->SetArrayField(Field, Arr);
}

void FMCPJson::WriteColor(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FLinearColor& C)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(C.R));
	Arr.Add(MakeShared<FJsonValueNumber>(C.G));
	Arr.Add(MakeShared<FJsonValueNumber>(C.B));
	Arr.Add(MakeShared<FJsonValueNumber>(C.A));
	Out->SetArrayField(Field, Arr);
}

void FMCPJson::WriteActor(const TSharedPtr<FJsonObject>& Out, AActor* Actor)
{
	if (!Actor) return;

	Out->SetStringField(TEXT("name"), Actor->GetActorLabel());
	Out->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	WriteVec3(Out, TEXT("location"), Actor->GetActorLocation());
	WriteRotator(Out, TEXT("rotation"), Actor->GetActorRotation());
	WriteVec3(Out, TEXT("scale"), Actor->GetActorScale3D());

	TArray<TSharedPtr<FJsonValue>> TagsArr;
	for (const FName& Tag : Actor->Tags)
	{
		TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Out->SetArrayField(TEXT("tags"), TagsArr);
}

TSharedPtr<FJsonObject> FMCPJson::MakeSuccess()
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	return Obj;
}

TSharedPtr<FJsonObject> FMCPJson::MakeError(const FString& Message)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), false);
	Obj->SetStringField(TEXT("error"), Message);
	return Obj;
}

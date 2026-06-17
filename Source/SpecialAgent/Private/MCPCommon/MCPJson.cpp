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

namespace
{
	// Read Arr[Index] as a number. Uses TryGetNumber (not AsNumber): numbers and
	// numeric strings succeed, but a non-numeric value (object/null/"abc") fails
	// instead of silently coercing to 0 — which would otherwise accept a bad
	// coordinate like ["100", 50, 0] and place an actor at the origin.
	bool ReadNumericElement(const TArray<TSharedPtr<FJsonValue>>& Arr, int32 Index, double& Out)
	{
		return Arr.IsValidIndex(Index) && Arr[Index].IsValid() && Arr[Index]->TryGetNumber(Out);
	}
}

bool FMCPJson::ReadVec3(const TSharedPtr<FJsonObject>& Params, const FString& Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(Field, Arr) || Arr->Num() != 3)
	{
		return false;
	}
	double X = 0.0, Y = 0.0, Z = 0.0;
	if (!ReadNumericElement(*Arr, 0, X) || !ReadNumericElement(*Arr, 1, Y) || !ReadNumericElement(*Arr, 2, Z))
	{
		return false;
	}
	Out = FVector(X, Y, Z);
	return true;
}

bool FMCPJson::ReadRotator(const TSharedPtr<FJsonObject>& Params, const FString& Field, FRotator& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(Field, Arr) || Arr->Num() != 3)
	{
		return false;
	}
	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	if (!ReadNumericElement(*Arr, 0, Pitch) || !ReadNumericElement(*Arr, 1, Yaw) || !ReadNumericElement(*Arr, 2, Roll))
	{
		return false;
	}
	Out = FRotator(Pitch, Yaw, Roll);
	return true;
}

bool FMCPJson::ReadColor(const TSharedPtr<FJsonObject>& Params, const FString& Field, FLinearColor& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Params->TryGetArrayField(Field, Arr) || Arr->Num() < 3)
	{
		return false;
	}
	double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
	if (!ReadNumericElement(*Arr, 0, R) || !ReadNumericElement(*Arr, 1, G) || !ReadNumericElement(*Arr, 2, B))
	{
		return false;
	}
	if (Arr->Num() >= 4 && !ReadNumericElement(*Arr, 3, A))
	{
		return false;
	}
	Out = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
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

	// `name` is kept for backward compatibility; `actor_label` is the preferred,
	// self-describing handle to pass to follow-up tools (both hold the editor
	// label). `path` is the full object path for unambiguous resolution.
	const FString Label = Actor->GetActorLabel();
	Out->SetStringField(TEXT("name"), Label);
	Out->SetStringField(TEXT("actor_label"), Label);
	Out->SetStringField(TEXT("path"), Actor->GetPathName());
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

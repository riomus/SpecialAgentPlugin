// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MCPCommon/MCPJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Wrap an array of JSON values under a field name into a params object.
	TSharedPtr<FJsonObject> ParamsWithArray(const FString& Field, TArray<TSharedPtr<FJsonValue>> Arr)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetArrayField(Field, MoveTemp(Arr));
		return O;
	}

	TSharedPtr<FJsonValue> Num(double V) { return MakeShared<FJsonValueNumber>(V); }
}

// ---------------------------------------------------------------------------
// FMCPJson::ReadVec3 — accepts valid numeric arrays, rejects malformed input
// instead of silently coercing bad elements to 0 (the robustness fix).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonReadVec3Valid,
	"SpecialAgent.Json.ReadVec3.Valid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPJsonReadVec3Valid::RunTest(const FString&)
{
	TArray<TSharedPtr<FJsonValue>> Arr = { Num(100.0), Num(-50.5), Num(0.0) };
	const TSharedPtr<FJsonObject> P = ParamsWithArray(TEXT("location"), Arr);

	FVector V(FVector::ZeroVector);
	TestTrue(TEXT("ReadVec3 succeeds on a numeric [X,Y,Z]"), FMCPJson::ReadVec3(P, TEXT("location"), V));
	TestEqual(TEXT("X"), V.X, 100.0);
	TestEqual(TEXT("Y"), V.Y, -50.5);
	TestEqual(TEXT("Z"), V.Z, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonReadVec3RejectsNonNumeric,
	"SpecialAgent.Json.ReadVec3.RejectsNonNumeric",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPJsonReadVec3RejectsNonNumeric::RunTest(const FString&)
{
	// A nested object is unambiguously non-numeric. Before the fix this was
	// silently coerced to 0.0 and accepted; now it must be rejected.
	TArray<TSharedPtr<FJsonValue>> Arr = {
		MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()),
		Num(50.0),
		Num(0.0)
	};
	const TSharedPtr<FJsonObject> P = ParamsWithArray(TEXT("location"), Arr);

	FVector V(FVector::ZeroVector);
	TestFalse(TEXT("ReadVec3 rejects a non-numeric element"), FMCPJson::ReadVec3(P, TEXT("location"), V));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonReadVec3RejectsWrongLength,
	"SpecialAgent.Json.ReadVec3.RejectsWrongLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPJsonReadVec3RejectsWrongLength::RunTest(const FString&)
{
	TArray<TSharedPtr<FJsonValue>> Arr = { Num(1.0), Num(2.0) };  // only 2 elements
	const TSharedPtr<FJsonObject> P = ParamsWithArray(TEXT("location"), Arr);

	FVector V(FVector::ZeroVector);
	TestFalse(TEXT("ReadVec3 rejects an array that is not length 3"), FMCPJson::ReadVec3(P, TEXT("location"), V));
	return true;
}

// ---------------------------------------------------------------------------
// FMCPJson::ReadColor — 3 or 4 elements; alpha defaults to 1; bad alpha rejected.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonReadColorRGBDefaultsAlpha,
	"SpecialAgent.Json.ReadColor.RGBDefaultsAlpha",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPJsonReadColorRGBDefaultsAlpha::RunTest(const FString&)
{
	TArray<TSharedPtr<FJsonValue>> Arr = { Num(0.25), Num(0.5), Num(0.75) };
	const TSharedPtr<FJsonObject> P = ParamsWithArray(TEXT("color"), Arr);

	FLinearColor C(ForceInit);
	TestTrue(TEXT("ReadColor succeeds on a 3-element RGB"), FMCPJson::ReadColor(P, TEXT("color"), C));
	TestEqual(TEXT("R"), C.R, 0.25f);
	TestEqual(TEXT("G"), C.G, 0.5f);
	TestEqual(TEXT("B"), C.B, 0.75f);
	TestEqual(TEXT("A defaults to 1"), C.A, 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonReadColorRejectsBadAlpha,
	"SpecialAgent.Json.ReadColor.RejectsBadAlpha",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPJsonReadColorRejectsBadAlpha::RunTest(const FString&)
{
	TArray<TSharedPtr<FJsonValue>> Arr = {
		Num(1.0), Num(1.0), Num(1.0),
		MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())  // non-numeric alpha
	};
	const TSharedPtr<FJsonObject> P = ParamsWithArray(TEXT("color"), Arr);

	FLinearColor C(ForceInit);
	TestFalse(TEXT("ReadColor rejects a non-numeric 4th (alpha) element"), FMCPJson::ReadColor(P, TEXT("color"), C));
	return true;
}

// ---------------------------------------------------------------------------
// FMCPJson::ReadInteger — truncates a JSON number to int32; absent field fails.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPJsonReadInteger,
	"SpecialAgent.Json.ReadInteger.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPJsonReadInteger::RunTest(const FString&)
{
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetNumberField(TEXT("count"), 42.9);

	int32 N = 0;
	TestTrue(TEXT("ReadInteger reads a present number"), FMCPJson::ReadInteger(P, TEXT("count"), N));
	TestEqual(TEXT("truncated to int32"), N, 42);

	int32 Missing = -1;
	TestFalse(TEXT("ReadInteger fails on an absent field"), FMCPJson::ReadInteger(P, TEXT("nope"), Missing));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

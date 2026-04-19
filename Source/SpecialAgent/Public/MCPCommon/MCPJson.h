#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;

class SPECIALAGENT_API FMCPJson
{
public:
	// ---- Readers ----
	// Return true if the field is present AND matches the expected shape.
	// Leave Out unchanged on failure so callers can supply defaults safely.

	static bool ReadString (const TSharedPtr<FJsonObject>& Params, const FString& Field, FString&     Out);
	static bool ReadNumber (const TSharedPtr<FJsonObject>& Params, const FString& Field, double&      Out);
	static bool ReadInteger(const TSharedPtr<FJsonObject>& Params, const FString& Field, int32&       Out);
	static bool ReadBool   (const TSharedPtr<FJsonObject>& Params, const FString& Field, bool&        Out);
	static bool ReadVec3   (const TSharedPtr<FJsonObject>& Params, const FString& Field, FVector&     Out);
	static bool ReadRotator(const TSharedPtr<FJsonObject>& Params, const FString& Field, FRotator&    Out);
	static bool ReadColor  (const TSharedPtr<FJsonObject>& Params, const FString& Field, FLinearColor&Out);

	// ---- Writers ----
	static void WriteVec3   (const TSharedPtr<FJsonObject>& Out, const FString& Field, const FVector&     V);
	static void WriteRotator(const TSharedPtr<FJsonObject>& Out, const FString& Field, const FRotator&    R);
	static void WriteColor  (const TSharedPtr<FJsonObject>& Out, const FString& Field, const FLinearColor&C);

	// Standard actor serialization. Matches the legacy SerializeActor in
	// WorldService.cpp so existing MCP clients keep parsing responses.
	// Emits: name (string), class (string), location [3], rotation [3],
	//        scale [3], tags (string[]).
	static void WriteActor  (const TSharedPtr<FJsonObject>& Out, AActor* Actor);

	// ---- Standard result shapes ----
	static TSharedPtr<FJsonObject> MakeSuccess();                         // {"success": true}
	static TSharedPtr<FJsonObject> MakeError(const FString& Message);     // {"success": false, "error": Message}
};

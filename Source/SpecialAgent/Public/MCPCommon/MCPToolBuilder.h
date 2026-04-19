#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"   // for FMCPToolInfo

/**
 * Fluent builder for FMCPToolInfo.
 *
 * Compresses the repetitive JSON-schema boilerplate that every
 * IMCPService::GetAvailableTools() must emit into a chain of typed calls:
 *
 *   FMCPToolInfo Tool = FMCPToolBuilder("spawn_actor", "Spawn an actor...")
 *       .RequiredString("actor_class", "Asset path or class name")
 *       .RequiredVec3("location", "Spawn location [X, Y, Z]")
 *       .OptionalVec3("rotation", "Rotation [Pitch, Yaw, Roll]")
 *       .Build();
 */
class SPECIALAGENT_API FMCPToolBuilder
{
public:
	FMCPToolBuilder(const FString& Name, const FString& Description);

	// Scalars
	FMCPToolBuilder& RequiredString (const FString& Field, const FString& Description);
	FMCPToolBuilder& RequiredNumber (const FString& Field, const FString& Description);
	FMCPToolBuilder& RequiredInteger(const FString& Field, const FString& Description);
	FMCPToolBuilder& RequiredBool   (const FString& Field, const FString& Description);

	FMCPToolBuilder& OptionalString (const FString& Field, const FString& Description);
	FMCPToolBuilder& OptionalNumber (const FString& Field, const FString& Description);
	FMCPToolBuilder& OptionalInteger(const FString& Field, const FString& Description);
	FMCPToolBuilder& OptionalBool   (const FString& Field, const FString& Description);

	// Compound (backed by JSON arrays with shape hints in description)
	FMCPToolBuilder& RequiredVec3  (const FString& Field, const FString& Description);
	FMCPToolBuilder& OptionalVec3  (const FString& Field, const FString& Description);
	FMCPToolBuilder& RequiredColor (const FString& Field, const FString& Description);
	FMCPToolBuilder& OptionalColor (const FString& Field, const FString& Description);

	// Enumerations
	FMCPToolBuilder& RequiredEnum(const FString& Field, const TArray<FString>& Allowed, const FString& Description);
	FMCPToolBuilder& OptionalEnum(const FString& Field, const TArray<FString>& Allowed, const FString& Description);

	// Array of strings
	FMCPToolBuilder& RequiredArrayOfString(const FString& Field, const FString& Description);
	FMCPToolBuilder& OptionalArrayOfString(const FString& Field, const FString& Description);

	FMCPToolInfo Build() const;

private:
	FMCPToolInfo Tool;

	// Single internal path: JsonType is one of "string", "number", "integer",
	// "boolean", "array". EnumValues is optional (null for non-enums).
	void AddParam(const FString& Field,
	              const FString& JsonType,
	              const FString& Description,
	              bool bRequired,
	              const TArray<FString>* EnumValues = nullptr);
};

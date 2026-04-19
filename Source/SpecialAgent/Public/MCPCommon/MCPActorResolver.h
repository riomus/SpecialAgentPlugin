#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;
class UWorld;

class SPECIALAGENT_API FMCPActorResolver
{
public:
	// Find an actor whose GetActorLabel() matches Name exactly.
	// Returns nullptr if no match or World is null. Matches the existing
	// FindActor semantics in WorldService.cpp.
	static AActor* ByLabel(UWorld* World, const FString& Name);

	// Find an actor by label OR by FName path (Actor->GetPathName()).
	// Tries ByLabel first, falls back to path match. Useful for clients
	// that don't know whether they have a label or a path.
	static AActor* ByLabelOrPath(UWorld* World, const FString& NameOrPath);

	// All actors of a given UClass (exact or subclass).
	static TArray<AActor*> ByClass(UWorld* World, UClass* Class);

	// All actors carrying the given tag (Actor->Tags.Contains(Tag)).
	static TArray<AActor*> ByTag(UWorld* World, const FName& Tag);
};

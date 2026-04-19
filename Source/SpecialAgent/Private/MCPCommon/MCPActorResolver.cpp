#include "MCPCommon/MCPActorResolver.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

AActor* FMCPActorResolver::ByLabel(UWorld* World, const FString& Name)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetActorLabel() == Name) return *It;
	}
	return nullptr;
}

AActor* FMCPActorResolver::ByLabelOrPath(UWorld* World, const FString& NameOrPath)
{
	if (AActor* ByL = ByLabel(World, NameOrPath)) return ByL;
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetPathName() == NameOrPath) return *It;
	}
	return nullptr;
}

TArray<AActor*> FMCPActorResolver::ByClass(UWorld* World, UClass* Class)
{
	TArray<AActor*> Out;
	if (!World || !Class) return Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->IsA(Class)) Out.Add(*It);
	}
	return Out;
}

TArray<AActor*> FMCPActorResolver::ByTag(UWorld* World, const FName& Tag)
{
	TArray<AActor*> Out;
	if (!World) return Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->Tags.Contains(Tag)) Out.Add(*It);
	}
	return Out;
}

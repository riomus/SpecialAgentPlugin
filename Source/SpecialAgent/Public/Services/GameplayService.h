// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Gameplay Service
 *
 * Spawn gameplay-related actors such as trigger volumes, player starts,
 * notes, target points, kill-Z volumes, and blocking volumes.
 *
 * Methods: spawn_trigger_volume, spawn_player_start, spawn_note,
 *          spawn_target_point, spawn_killz_volume, spawn_blocking_volume.
 */
class SPECIALAGENT_API FGameplayService : public IMCPService
{
public:
	FGameplayService();
	virtual ~FGameplayService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSpawnTriggerVolume(const FMCPRequest& Request);
	FMCPResponse HandleSpawnPlayerStart(const FMCPRequest& Request);
	FMCPResponse HandleSpawnNote(const FMCPRequest& Request);
	FMCPResponse HandleSpawnTargetPoint(const FMCPRequest& Request);
	FMCPResponse HandleSpawnKillZVolume(const FMCPRequest& Request);
	FMCPResponse HandleSpawnBlockingVolume(const FMCPRequest& Request);
};

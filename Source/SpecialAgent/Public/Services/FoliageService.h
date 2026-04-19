// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Foliage Service
 *
 * Procedural foliage painting and management.
 * Methods: paint_in_area, remove_from_area, get_density,
 *          list_foliage_types, add_foliage_type
 */
class SPECIALAGENT_API FFoliageService : public IMCPService
{
public:
	FFoliageService();
	virtual ~FFoliageService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandlePaintInArea(const FMCPRequest& Request);
	FMCPResponse HandleRemoveFromArea(const FMCPRequest& Request);
	FMCPResponse HandleGetDensity(const FMCPRequest& Request);
	FMCPResponse HandleListFoliageTypes(const FMCPRequest& Request);
	FMCPResponse HandleAddFoliageType(const FMCPRequest& Request);
};

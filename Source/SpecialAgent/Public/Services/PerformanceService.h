// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Performance Service
 *
 * Level performance analysis and optimization.
 *
 * Methods: get_statistics, get_actor_bounds, check_overlaps,
 *          get_triangle_count, get_draw_call_estimate.
 */
class SPECIALAGENT_API FPerformanceService : public IMCPService
{
public:
	FPerformanceService();
	virtual ~FPerformanceService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleGetStatistics(const FMCPRequest& Request);
	FMCPResponse HandleGetActorBounds(const FMCPRequest& Request);
	FMCPResponse HandleCheckOverlaps(const FMCPRequest& Request);
	FMCPResponse HandleGetTriangleCount(const FMCPRequest& Request);
	FMCPResponse HandleGetDrawCallEstimate(const FMCPRequest& Request);
};

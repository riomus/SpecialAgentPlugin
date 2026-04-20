#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * WorldPartition Service.
 *
 * World Partition cell loading and streaming.
 * Methods: list_cells, load_cell, unload_cell, get_loaded_cells,
 *          force_load_region.
 */
class SPECIALAGENT_API FWorldPartitionService : public IMCPService
{
public:
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleListCells(const FMCPRequest& Request);
	FMCPResponse HandleLoadCell(const FMCPRequest& Request);
	FMCPResponse HandleUnloadCell(const FMCPRequest& Request);
	FMCPResponse HandleGetLoadedCells(const FMCPRequest& Request);
	FMCPResponse HandleForceLoadRegion(const FMCPRequest& Request);
};

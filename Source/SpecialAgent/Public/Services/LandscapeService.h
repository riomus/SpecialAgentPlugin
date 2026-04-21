// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Landscape Service
 *
 * Terrain sculpting and material layer painting.
 * Methods: get_info, sculpt_height, flatten_area, smooth_area,
 *          paint_layer, list_layers
 */
class SPECIALAGENT_API FLandscapeService : public IMCPService
{
public:
	FLandscapeService();
	virtual ~FLandscapeService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleGetInfo(const FMCPRequest& Request);
	FMCPResponse HandleSculptHeight(const FMCPRequest& Request);
	FMCPResponse HandleFlattenArea(const FMCPRequest& Request);
	FMCPResponse HandleSmoothArea(const FMCPRequest& Request, const FMCPRequestContext& Ctx);
	FMCPResponse HandlePaintLayer(const FMCPRequest& Request);
	FMCPResponse HandleListLayers(const FMCPRequest& Request);
};

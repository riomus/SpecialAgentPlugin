// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Asset Service
 *
 * Asset discovery, query, and management.
 *
 * Query tools:     list, find, get_properties, search, get_bounds, get_info.
 * Management tools: sync_to_browser, create_folder, rename, delete, move,
 *                   duplicate, save, set_metadata, get_metadata, validate.
 */
class SPECIALAGENT_API FAssetService : public IMCPService
{
public:
	FAssetService();
	virtual ~FAssetService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	// Existing — query/discovery handlers.
	FMCPResponse HandleListAssets(const FMCPRequest& Request);
	FMCPResponse HandleFindAsset(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetProperties(const FMCPRequest& Request);
	FMCPResponse HandleSearchAssets(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetBounds(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetInfo(const FMCPRequest& Request);

	// New — management handlers.
	FMCPResponse HandleSyncToBrowser(const FMCPRequest& Request);
	FMCPResponse HandleCreateFolder(const FMCPRequest& Request);
	FMCPResponse HandleRenameAsset(const FMCPRequest& Request);
	FMCPResponse HandleDeleteAsset(const FMCPRequest& Request);
	FMCPResponse HandleMoveAsset(const FMCPRequest& Request);
	FMCPResponse HandleDuplicateAsset(const FMCPRequest& Request);
	FMCPResponse HandleSaveAsset(const FMCPRequest& Request);
	FMCPResponse HandleSetMetadata(const FMCPRequest& Request);
	FMCPResponse HandleGetMetadata(const FMCPRequest& Request);
	FMCPResponse HandleValidateAsset(const FMCPRequest& Request);
};

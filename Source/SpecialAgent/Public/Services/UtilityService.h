// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Utility Service
 *
 * Editor utility operations like save, undo/redo, selection management,
 * transactions, notifications, and tab/browser focus.
 */
class SPECIALAGENT_API FUtilityService : public IMCPService
{
public:
	FUtilityService();
	virtual ~FUtilityService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSaveLevel(const FMCPRequest& Request);
	FMCPResponse HandleUndo(const FMCPRequest& Request);
	FMCPResponse HandleRedo(const FMCPRequest& Request);
	FMCPResponse HandleSelectActor(const FMCPRequest& Request);
	FMCPResponse HandleGetSelection(const FMCPRequest& Request);
	FMCPResponse HandleGetSelectionBounds(const FMCPRequest& Request);
	FMCPResponse HandleSelectAtScreen(const FMCPRequest& Request);

	// Phase 1.A additions
	FMCPResponse HandleFocusAssetInBrowser(const FMCPRequest& Request);
	FMCPResponse HandleDeselectAll(const FMCPRequest& Request);
	FMCPResponse HandleInvertSelection(const FMCPRequest& Request);
	FMCPResponse HandleSelectByClass(const FMCPRequest& Request);
	FMCPResponse HandleGroupSelected(const FMCPRequest& Request);
	FMCPResponse HandleUngroup(const FMCPRequest& Request);
	FMCPResponse HandleBeginTransaction(const FMCPRequest& Request);
	FMCPResponse HandleEndTransaction(const FMCPRequest& Request);
	FMCPResponse HandleShowNotification(const FMCPRequest& Request);
	FMCPResponse HandleShowDialog(const FMCPRequest& Request);
	FMCPResponse HandleFocusTab(const FMCPRequest& Request);
};


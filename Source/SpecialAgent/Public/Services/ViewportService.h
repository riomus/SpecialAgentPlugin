// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Viewport Service
 *
 * Control editor viewport camera for optimal screenshot capture, view-mode
 * switching, FOV/bookmark control, and grid snapping.
 */
class SPECIALAGENT_API FViewportService : public IMCPService
{
public:
	FViewportService();
	virtual ~FViewportService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSetLocation(const FMCPRequest& Request);
	FMCPResponse HandleSetRotation(const FMCPRequest& Request);
	FMCPResponse HandleGetTransform(const FMCPRequest& Request);
	FMCPResponse HandleFocusActor(const FMCPRequest& Request);
	FMCPResponse HandleTraceFromScreen(const FMCPRequest& Request);

	// Phase 1.A additions
	FMCPResponse HandleOrbitAroundActor(const FMCPRequest& Request);
	FMCPResponse HandleSetFov(const FMCPRequest& Request);
	FMCPResponse HandleSetViewMode(const FMCPRequest& Request);
	FMCPResponse HandleToggleGameView(const FMCPRequest& Request);
	FMCPResponse HandleBookmarkSave(const FMCPRequest& Request);
	FMCPResponse HandleBookmarkRestore(const FMCPRequest& Request);
	FMCPResponse HandleSetGridSnap(const FMCPRequest& Request);
	FMCPResponse HandleToggleRealtime(const FMCPRequest& Request);
	FMCPResponse HandleForceRedraw(const FMCPRequest& Request);
};


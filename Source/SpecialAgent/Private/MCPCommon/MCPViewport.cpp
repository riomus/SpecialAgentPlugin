#include "MCPCommon/MCPViewport.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "UnrealClient.h"

FLevelEditorViewportClient* FMCPViewport::GetLevelClient()
{
	if (!GEditor)
	{
		return nullptr;
	}

	const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();

	// 1. Prefer the viewport the user last interacted with, but only if it is
	//    still a live entry in the editor's level-viewport list (guards against
	//    a dangling global after a layout change).
	if (GCurrentLevelEditingViewportClient && Clients.Contains(GCurrentLevelEditingViewportClient))
	{
		return GCurrentLevelEditingViewportClient;
	}

	// 2. Otherwise pick a perspective level viewport, falling back to any.
	FLevelEditorViewportClient* Fallback = nullptr;
	for (FLevelEditorViewportClient* Client : Clients)
	{
		if (!Client)
		{
			continue;
		}
		if (!Fallback)
		{
			Fallback = Client;
		}
		if (Client->IsPerspective())
		{
			return Client;
		}
	}
	return Fallback;
}

FViewport* FMCPViewport::GetViewportForCapture(bool bForceRedraw)
{
	FLevelEditorViewportClient* Client = GetLevelClient();
	if (!Client)
	{
		return nullptr;
	}

	if (bForceRedraw && GEditor)
	{
		// Commit any queued camera/view/scene changes to pixels before the
		// caller reads them. Without this the non-realtime editor viewport
		// hands back a stale (often black) frame, which is the single biggest
		// cause of "screenshots show the previous state".
		Client->Invalidate();
		GEditor->RedrawLevelEditingViewports(true);
	}

	return Client->Viewport;
}

#pragma once

#include "CoreMinimal.h"

class FLevelEditorViewportClient;
class FViewport;

/**
 * Robust resolution of the Level Editor viewport for screenshots and camera
 * control.
 *
 * Why this exists: the rest of the plugin historically reached the viewport via
 *   static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient())
 * which has two defects:
 *   1. GetActiveViewport() returns whatever viewport currently has focus — a
 *      material / Niagara / preview / thumbnail viewport, or null. Casting a
 *      non-level client to FLevelEditorViewportClient* and calling methods on it
 *      is undefined behaviour and crashes the editor.
 *   2. GetActiveViewport() is frequently null when the editor is driven headless
 *      over MCP (no level viewport focused), dereferencing null.
 *
 * These helpers instead resolve through GEditor's typed level-viewport list, so
 * the pointer is always a genuine FLevelEditorViewportClient* or nullptr.
 *
 * All methods MUST be called on the game thread.
 */
class SPECIALAGENT_API FMCPViewport
{
public:
	/** Best level-editor viewport client (prefers the last-used perspective
	 *  viewport), or nullptr if none exists. Never returns a non-level client. */
	static FLevelEditorViewportClient* GetLevelClient();

	/**
	 * Resolve the level viewport and, when bForceRedraw is true, synchronously
	 * repaint it so a subsequent pixel read reflects queued camera/scene changes
	 * (editor viewports are not realtime, so the backbuffer is otherwise stale).
	 * Returns the FViewport* to read from, or nullptr.
	 */
	static FViewport* GetViewportForCapture(bool bForceRedraw = true);
};

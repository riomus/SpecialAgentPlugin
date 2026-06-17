// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ScreenshotService.h"
#include "GameThreadDispatcher.h"
#include "MCPCommon/MCPRequestContext.h"
#include "MCPCommon/MCPViewport.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"

FScreenshotService::FScreenshotService()
{
}

FString FScreenshotService::GetServiceDescription() const
{
	return TEXT("Screenshot capture - CRITICAL visual feedback for iterative design");
}

TArray<FMCPToolInfo> FScreenshotService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// capture
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("capture");
		Tool.Description = TEXT("Capture the Level Editor viewport as an in-memory image for inline vision (no file written). "
			"Returns {success, width, height, quality, data_size} plus {base64_data, mimeType} when return_base64 is true (mimeType is image/jpeg for quality 1-99, image/png for quality 100). "
			"Params: width (integer px, default 1280); height (integer px, default 720); quality (integer 1-100, JPEG below 100 else lossless PNG, default 85); return_base64 (bool, default true; false omits base64_data and just reports size); force_redraw (bool, default true). "
			"Workflow: ALWAYS call first for visual feedback, then estimate (x,y) in 0-1 screen space for viewport/trace_from_screen or utility/select_at_screen; call again after edits to verify. "
			"Warning: this auto-redraws by default (synchronously repaints the Level Editor viewport before reading pixels), so the frame already reflects the latest camera/scene state after set_location/set_rotation/focus_actor/orbit/set_fov/set_view_mode/toggle_game_view or python camera edits -- a separate viewport/force_redraw is NOT needed; pass force_redraw:false only to skip the repaint for speed. Always targets the Level Editor viewport (never a focused material/Niagara/preview window). Large width/height produce huge base64 payloads, so prefer the defaults.");

		TSharedPtr<FJsonObject> WidthParam = MakeShared<FJsonObject>();
		WidthParam->SetStringField(TEXT("type"), TEXT("number"));
		WidthParam->SetStringField(TEXT("description"), TEXT("Image width in pixels (default: 1280)"));
		Tool.Parameters->SetObjectField(TEXT("width"), WidthParam);

		TSharedPtr<FJsonObject> HeightParam = MakeShared<FJsonObject>();
		HeightParam->SetStringField(TEXT("type"), TEXT("number"));
		HeightParam->SetStringField(TEXT("description"), TEXT("Image height in pixels (default: 720)"));
		Tool.Parameters->SetObjectField(TEXT("height"), HeightParam);

		TSharedPtr<FJsonObject> QualityParam = MakeShared<FJsonObject>();
		QualityParam->SetStringField(TEXT("type"), TEXT("number"));
		QualityParam->SetStringField(TEXT("description"), TEXT("JPEG quality 1-99, or 100 for lossless PNG (default: 85)"));
		Tool.Parameters->SetObjectField(TEXT("quality"), QualityParam);

		TSharedPtr<FJsonObject> Base64Param = MakeShared<FJsonObject>();
		Base64Param->SetStringField(TEXT("type"), TEXT("boolean"));
		Base64Param->SetStringField(TEXT("description"), TEXT("Include the encoded image as base64_data/mimeType in the result (default: true). Set false to only report width/height/data_size without the payload."));
		Tool.Parameters->SetObjectField(TEXT("return_base64"), Base64Param);

		TSharedPtr<FJsonObject> RedrawParam = MakeShared<FJsonObject>();
		RedrawParam->SetStringField(TEXT("type"), TEXT("boolean"));
		RedrawParam->SetStringField(TEXT("description"), TEXT("Repaint the viewport before reading pixels so the frame is fresh (default: true). Set false to skip for speed."));
		Tool.Parameters->SetObjectField(TEXT("force_redraw"), RedrawParam);

		Tools.Add(Tool);
	}
	
	// save
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("save");
		Tool.Description = TEXT("Capture the Level Editor viewport and write it to a lossless PNG file on disk. Returns {success, file_path, width, height, file_size}. "
			"Params: file_path (string, absolute OS path, required); width (integer px, default 1920); height (integer px, default 1080); force_redraw (bool, default true). "
			"Workflow: use instead of screenshot/capture when you want a persisted PNG artifact rather than inline base64; pass an absolute OS path (NOT a /Game virtual path). "
			"Warning: this auto-redraws by default (synchronously repaints the Level Editor viewport before reading pixels), so the saved frame reflects the latest camera/scene state and a separate viewport/force_redraw is NOT needed. The parent directory of file_path must already exist; an existing file at file_path is overwritten without prompting.");

		TSharedPtr<FJsonObject> FileParam = MakeShared<FJsonObject>();
		FileParam->SetStringField(TEXT("type"), TEXT("string"));
		FileParam->SetStringField(TEXT("description"), TEXT("Absolute OS file path to save the PNG screenshot to (parent directory must exist; not a /Game virtual path)"));
		Tool.Parameters->SetObjectField(TEXT("file_path"), FileParam);
		Tool.RequiredParams.Add(TEXT("file_path"));

		TSharedPtr<FJsonObject> SaveWidthParam = MakeShared<FJsonObject>();
		SaveWidthParam->SetStringField(TEXT("type"), TEXT("number"));
		SaveWidthParam->SetStringField(TEXT("description"), TEXT("Image width in pixels (default: 1920)"));
		Tool.Parameters->SetObjectField(TEXT("width"), SaveWidthParam);

		TSharedPtr<FJsonObject> SaveHeightParam = MakeShared<FJsonObject>();
		SaveHeightParam->SetStringField(TEXT("type"), TEXT("number"));
		SaveHeightParam->SetStringField(TEXT("description"), TEXT("Image height in pixels (default: 1080)"));
		Tool.Parameters->SetObjectField(TEXT("height"), SaveHeightParam);

		TSharedPtr<FJsonObject> SaveRedrawParam = MakeShared<FJsonObject>();
		SaveRedrawParam->SetStringField(TEXT("type"), TEXT("boolean"));
		SaveRedrawParam->SetStringField(TEXT("description"), TEXT("Repaint the viewport before reading pixels (default: true)."));
		Tool.Parameters->SetObjectField(TEXT("force_redraw"), SaveRedrawParam);

		Tools.Add(Tool);
	}
	
	return Tools;
}

FMCPResponse FScreenshotService::HandleRequest(const FMCPRequest& Request, const FString& MethodName, const FMCPRequestContext& Ctx)
{
	if (MethodName == TEXT("capture")) return HandleCapture(Request);
	if (MethodName == TEXT("save")) return HandleSave(Request);

	return MethodNotFound(Request.Id, TEXT("screenshot"), MethodName);
}

FMCPResponse FScreenshotService::HandleCapture(const FMCPRequest& Request)
{
	// Get parameters - smaller defaults to avoid huge base64 strings causing client hangs
	int32 Width = 1280;
	int32 Height = 720;
	int32 Quality = 85;  // JPEG quality (1-100), use 100 for PNG
	bool bReturnBase64 = true;
	bool bForceRedraw = true;  // repaint the viewport before reading so the frame is fresh

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("width"), Width);
		Request.Params->TryGetNumberField(TEXT("height"), Height);
		Request.Params->TryGetNumberField(TEXT("quality"), Quality);
		Request.Params->TryGetBoolField(TEXT("return_base64"), bReturnBase64);
		Request.Params->TryGetBoolField(TEXT("force_redraw"), bForceRedraw);
	}

	// Clamp quality to valid range
	Quality = FMath::Clamp(Quality, 1, 100);

	// Capture on game thread
	auto CaptureTask = [Width, Height, Quality, bReturnBase64, bForceRedraw]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Resolve the level-editor viewport (never a focused material/preview
		// viewport) and, by default, force a synchronous repaint so the frame
		// reflects any queued camera/scene change rather than a stale backbuffer.
		FViewport* Viewport = FMCPViewport::GetViewportForCapture(bForceRedraw);
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No Level Editor viewport available to capture"));
			return Result;
		}

		// Read pixels from viewport
		TArray<FColor> Bitmap;
		FIntPoint ViewportSize = Viewport->GetSizeXY();
		
		// Read the viewport pixels
		if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), FIntRect(0, 0, ViewportSize.X, ViewportSize.Y)))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to read viewport pixels"));
			return Result;
		}

		// Resize if requested size is different
		if (Width != ViewportSize.X || Height != ViewportSize.Y)
		{
			TArray<FColor> ResizedBitmap;
			FImageUtils::ImageResize(ViewportSize.X, ViewportSize.Y, Bitmap, Width, Height, ResizedBitmap, false);
			Bitmap = MoveTemp(ResizedBitmap);
			ViewportSize = FIntPoint(Width, Height);
		}

		// Use JPEG for smaller file sizes (quality < 100), PNG for lossless (quality = 100)
		EImageFormat ImageFormat = (Quality < 100) ? EImageFormat::JPEG : EImageFormat::PNG;
		FString MimeType = (Quality < 100) ? TEXT("image/jpeg") : TEXT("image/png");
		
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);

		if (!ImageWrapper.IsValid())
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to create image wrapper"));
			return Result;
		}

		// Set raw data
		if (!ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), ViewportSize.X, ViewportSize.Y, ERGBFormat::BGRA, 8))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to set raw image data"));
			return Result;
		}

		// Get compressed data
		TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(Quality);

		if (CompressedData.Num() == 0)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to compress image"));
			return Result;
		}

		// Encode to base64 if requested
		if (bReturnBase64)
		{
			FString Base64String = FBase64::Encode(CompressedData.GetData(), CompressedData.Num());
			Result->SetStringField(TEXT("base64_data"), Base64String);
			Result->SetStringField(TEXT("mimeType"), MimeType);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("width"), ViewportSize.X);
		Result->SetNumberField(TEXT("height"), ViewportSize.Y);
		Result->SetNumberField(TEXT("quality"), Quality);
		Result->SetNumberField(TEXT("data_size"), CompressedData.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screenshot captured: %dx%d, quality=%d, %lld bytes"), 
			ViewportSize.X, ViewportSize.Y, Quality, CompressedData.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(CaptureTask);

	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FScreenshotService::HandleSave(const FMCPRequest& Request)
{
	// Get parameters
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FilePath;
	if (!Request.Params->TryGetStringField(TEXT("file_path"), FilePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'file_path'"));
	}

	int32 Width = 1920;
	int32 Height = 1080;
	bool bForceRedraw = true;
	Request.Params->TryGetNumberField(TEXT("width"), Width);
	Request.Params->TryGetNumberField(TEXT("height"), Height);
	Request.Params->TryGetBoolField(TEXT("force_redraw"), bForceRedraw);

	// Capture and save on game thread
	auto SaveTask = [Width, Height, FilePath, bForceRedraw]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Resolve the level-editor viewport and force a fresh frame (see capture).
		FViewport* Viewport = FMCPViewport::GetViewportForCapture(bForceRedraw);
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No Level Editor viewport available to capture"));
			return Result;
		}

		// Read pixels
		TArray<FColor> Bitmap;
		FIntPoint ViewportSize = Viewport->GetSizeXY();
		
		if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), FIntRect(0, 0, ViewportSize.X, ViewportSize.Y)))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to read viewport pixels"));
			return Result;
		}

		// Resize if needed
		if (Width != ViewportSize.X || Height != ViewportSize.Y)
		{
			TArray<FColor> ResizedBitmap;
			FImageUtils::ImageResize(ViewportSize.X, ViewportSize.Y, Bitmap, Width, Height, ResizedBitmap, false);
			Bitmap = MoveTemp(ResizedBitmap);
			ViewportSize = FIntPoint(Width, Height);
		}

		// Convert to PNG
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

		if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), ViewportSize.X, ViewportSize.Y, ERGBFormat::BGRA, 8))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to prepare image"));
			return Result;
		}

		TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(100);

		// Save to file
		if (FFileHelper::SaveArrayToFile(CompressedData, *FilePath))
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("file_path"), FilePath);
			Result->SetNumberField(TEXT("width"), ViewportSize.X);
			Result->SetNumberField(TEXT("height"), ViewportSize.Y);
			Result->SetNumberField(TEXT("file_size"), CompressedData.Num());

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screenshot saved to: %s (%dx%d)"), 
				*FilePath, ViewportSize.X, ViewportSize.Y);
		}
		else
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to save file: %s"), *FilePath));
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SaveTask);

	return FMCPResponse::Success(Request.Id, Result);
}


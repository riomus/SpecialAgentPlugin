// Copyright Epic Games, Inc. All Rights Reserved.

#include "SpecialAgentModule.h"
#include "MCPServer.h"
#include "MCPStatusBarWidget.h"
#include "MCPCommon/MCPGameThreadProcessor.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FSpecialAgentModule"

void FSpecialAgentModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Module starting up"));

	// Create the MCP server instance
	MCPServer = MakeShared<FSpecialAgentMCPServer>();

	// Server settings. Defaults are used unless overridden in config.
	const TCHAR* SettingsSection = TEXT("/Script/SpecialAgent.SpecialAgentSettings");
	bool bAutoStart = true;     // ServerEnabled
	int32 ServerPort = 8767;    // HTTP/SSE port for MCP client connections

	if (GConfig)
	{
		// The plugin ships Config/DefaultSpecialAgent.ini (ServerEnabled / ServerPort).
		// Without a UCLASS(config=SpecialAgent) settings object it is not merged
		// into a standard config branch automatically, so load it explicitly by
		// path — otherwise the shipped config file has no effect.
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SpecialAgent")))
		{
			const FString PluginIni = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("DefaultSpecialAgent.ini"));
			if (FPaths::FileExists(PluginIni))
			{
				GConfig->GetBool(SettingsSection, TEXT("ServerEnabled"), bAutoStart, PluginIni);
				GConfig->GetInt (SettingsSection, TEXT("ServerPort"),    ServerPort,  PluginIni);
			}
		}

		// Project-level DefaultGame.ini overrides the plugin default when present.
		GConfig->GetBool(SettingsSection, TEXT("ServerEnabled"), bAutoStart, GGameIni);
		GConfig->GetInt (SettingsSection, TEXT("ServerPort"),    ServerPort,  GGameIni);
	}
	
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: ServerEnabled=%d, ServerPort=%d"), bAutoStart, ServerPort);

	if (bAutoStart)
	{
		if (MCPServer->StartServer(ServerPort))
		{
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: MCP Server started on port %d"), ServerPort);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("SpecialAgent: Failed to start MCP Server"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: MCP Server auto-start is disabled"));
	}

	// Register status bar widget (editor UI only — skip in commandlets like cook)
	if (!IsRunningCommandlet())
	{
		RegisterStatusBarWidget();

		// Keep the game thread ticking when the editor is not the foreground
		// window. The MCP transport marshals every editor operation onto the
		// game thread via the editor Tick(); with "Use Less CPU when in
		// Background" enabled (the editor default) that Tick is throttled to a
		// crawl whenever the editor loses focus — which is the normal case when
		// an external MCP client drives it. That throttling is the single
		// biggest cause of MCP requests appearing to hang. Disable it in-memory
		// for this session (no persisted change to the user's preferences).
		if (UEditorPerformanceSettings* PerfSettings = GetMutableDefault<UEditorPerformanceSettings>())
		{
			if (PerfSettings->bThrottleCPUWhenNotForeground)
			{
				PerfSettings->bThrottleCPUWhenNotForeground = false;
				PerfSettings->PostEditChange();
				UE_LOG(LogTemp, Log,
					TEXT("SpecialAgent: disabled 'Use Less CPU when in Background' so MCP requests are not throttled while the editor is backgrounded"));
			}
		}
	}

	// Touch the processor on the game thread so its FTickableEditorObject
	// registration happens on the right thread before any HTTP worker enqueues.
	FMCPGameThreadProcessor::Get();
}

void FSpecialAgentModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Module shutting down"));

	// Drain the game-thread processor and block subsequent Enqueue calls
	// before tearing anything else down. Any HTTP worker mid-Enqueue will
	// now fast-fail instead of hanging its .Get().
	FMCPGameThreadProcessor::Get().Shutdown();

	UnregisterStatusBarWidget();

	if (MCPServer.IsValid())
	{
		MCPServer->StopServer();
		MCPServer.Reset();
	}
}

bool FSpecialAgentModule::IsMCPServerRunning() const
{
	return MCPServer.IsValid() && MCPServer->IsRunning();
}

void FSpecialAgentModule::RegisterStatusBarWidget()
{
	// Use UToolMenus to add our status widget to the level editor status bar
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	// Register with the status bar
	const FName StatusBarName = TEXT("LevelEditor.StatusBar.ToolBar");
	UToolMenu* StatusBarMenu = ToolMenus->ExtendMenu(StatusBarName);
	
	if (StatusBarMenu)
	{
		TSharedPtr<FSpecialAgentMCPServer> Server = MCPServer;
		
		FToolMenuSection& Section = StatusBarMenu->FindOrAddSection(TEXT("SpecialAgent"));
		Section.AddEntry(FToolMenuEntry::InitWidget(
			TEXT("MCPStatus"),
			SNew(SMCPStatusBarWidget, Server),
			FText::GetEmpty(),
			true,  // bNoIndent
			false  // bSearchable
		));
		
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Status bar widget registered via ToolMenus"));
	}
	else
	{
		// Fallback: Register with level editor module directly
		if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
		{
			FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
			
			TSharedPtr<FSpecialAgentMCPServer> Server = MCPServer;
			ToolBarExtender = MakeShareable(new FExtender);
			
			ToolBarExtender->AddToolBarExtension(
				TEXT("SourceControl"),
				EExtensionHook::After,
				nullptr,
				FToolBarExtensionDelegate::CreateLambda([Server](FToolBarBuilder& Builder)
				{
					Builder.AddWidget(SNew(SMCPStatusBarWidget, Server));
				})
			);
			
			LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolBarExtender);
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Status bar widget registered via toolbar extender"));
		}
	}
}

void FSpecialAgentModule::UnregisterStatusBarWidget()
{
	// Remove from ToolMenus
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus)
	{
		const FName StatusBarName = TEXT("LevelEditor.StatusBar.ToolBar");
		UToolMenu* StatusBarMenu = ToolMenus->FindMenu(StatusBarName);
		if (StatusBarMenu)
		{
			StatusBarMenu->RemoveSection(TEXT("SpecialAgent"));
		}
	}

	// Remove toolbar extender if used
	if (ToolBarExtender.IsValid() && FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		LevelEditorModule.GetToolBarExtensibilityManager()->RemoveExtender(ToolBarExtender);
		ToolBarExtender.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSpecialAgentModule, SpecialAgent)

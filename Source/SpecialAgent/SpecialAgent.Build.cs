// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SpecialAgent : ModuleRules
{
	public SpecialAgent(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
		);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
		);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"HTTP",
				"Sockets",
				"Networking",
				"ApplicationCore",
			}
		);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"AssetRegistry",
				"EditorSubsystem",
				"LevelEditor",
				"PythonScriptPlugin",
				"EditorScriptingUtilities",
				"Slate",
				"SlateCore",
				"InputCore",
				"PropertyEditor",
				"Projects",
				"Foliage",
				"Landscape",
				"LandscapeEditor",
				"NavigationSystem",
				"AIModule",
				"ToolMenus",
				// Phase 0.6 expansion — module deps for the 32 new scaffolded services.
				// Any module that Phase 1 agents find doesn't exist in UE 5.6+ will be pruned then.
				"AssetTools",
				"InterchangeCore",
				"InterchangeEngine",
				"Blutility",
				"KismetCompiler",
				"BlueprintGraph",
				"UMG",
				"UMGEditor",
				"Niagara",
				"NiagaraEditor",
				"MovieScene",
				"MovieSceneTracks",
				"MovieSceneTools",
				"LevelSequence",
				"LevelSequenceEditor",
				"WorldPartitionEditor",
				"PCG",
				"PCGEditor",
				"SourceControl",
				"MovieRenderPipelineCore",
				"MovieRenderPipelineEditor",
				"EnhancedInput",
				"GeometryFramework",
				"GeometryScriptingCore",
				"MeshModelingTools",
				"ContentBrowserData",
				"ContentBrowser",
				"DataValidation",
				"MessageLog",
			}
		);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
}


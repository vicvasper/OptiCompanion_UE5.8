using UnrealBuildTool;

public class OptiCompanionEditor : ModuleRules
{
	public OptiCompanionEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"OptiCompanion",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AssetRegistry",
			"BlueprintGraph",
			"DesktopPlatform",
			"EditorFramework",
			"InputCore",
			"Json",
			"LevelEditor",
			"MainFrame",
			"MaterialEditor",
			"MeshDescription",
			"Projects",
			"Settings",
			"Slate",
			"SlateCore",
			"SourceControl",
			"StaticMeshDescription",
			"RenderCore",
			"ToolMenus",
			"TraceAnalysis",
			"TraceLog",
			"TraceServices",
			"UnrealEd",
		});
	}
}

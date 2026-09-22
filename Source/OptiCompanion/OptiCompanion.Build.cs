using UnrealBuildTool;

public class OptiCompanion : ModuleRules
{
	public OptiCompanion(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"ImageWrapper",
			"RHI",
			"RenderCore",
			"Projects",
		});
	}
}

using UnrealBuildTool;

public class BlueprintGraphExport : ModuleRules
{
	public BlueprintGraphExport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"BlueprintGraph",
				"CoreUObject",
				"DeveloperSettings",
				"EditorSubsystem",
				"Engine",
				"Json",
				"JsonUtilities",
				"Kismet",
				"KismetCompiler",
				"Projects",
				"UnrealEd"
			}
		);
	}
}

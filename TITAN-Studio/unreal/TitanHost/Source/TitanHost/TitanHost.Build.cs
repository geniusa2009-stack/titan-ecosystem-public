// TITAN Studio host project (original TITAN code).
using UnrealBuildTool;

public class TitanHost : ModuleRules
{
	public TitanHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}

using UnrealBuildTool;
public class TitanDataCore : ModuleRules
{
    public TitanDataCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnableExceptions = true; // core algorithms report invalid graphs/configuration
        PublicDependencyModuleNames.AddRange(new string[] { "Core" });
    }
}

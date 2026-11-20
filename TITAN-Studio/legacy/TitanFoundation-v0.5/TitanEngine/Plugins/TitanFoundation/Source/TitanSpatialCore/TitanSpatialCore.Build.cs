using UnrealBuildTool;
public class TitanSpatialCore : ModuleRules
{
    public TitanSpatialCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core" });
    }
}

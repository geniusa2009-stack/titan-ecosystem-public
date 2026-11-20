using UnrealBuildTool;
public class TitanFoundationRuntime : ModuleRules
{
    public TitanFoundationRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnableExceptions = true; // TitanECS.h has validated exception-throwing template operations
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "TitanSpatialCore", "TitanOrchestrationCore", "TitanDataCore" });
    }
}

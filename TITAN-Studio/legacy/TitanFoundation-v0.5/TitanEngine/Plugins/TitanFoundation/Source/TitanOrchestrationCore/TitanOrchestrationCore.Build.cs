using UnrealBuildTool;
public class TitanOrchestrationCore : ModuleRules
{
    public TitanOrchestrationCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core" });
        bEnableExceptions = true; // graph validation throws std::logic_error in tools/tests
        // Pure C++14 scheduling and graph code; no UObject dependency.
    }
}

// Original TITAN code.
using UnrealBuildTool;

public class TitanStudioBridge : ModuleRules
{
	public TitanStudioBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",     // GEditor, transactions, FEditorFileUtils, selection
			"Json",         // protocol messages
			"RenderCore",   // ENQUEUE_RENDER_COMMAND, render-thread timers
			"RHI",          // FRHIGPUTextureReadback, GPU frame time
			"Slate",        // minimise / restore the editor's windows
			"SlateCore",
		});
	}
}

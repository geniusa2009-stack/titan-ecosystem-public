// TITAN Studio host project (original TITAN code).
using UnrealBuildTool;

public class TitanHostEditorTarget : TargetRules
{
	public TitanHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("TitanHost");
	}
}

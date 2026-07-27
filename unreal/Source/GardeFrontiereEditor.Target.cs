using UnrealBuildTool;

public class GardeFrontiereEditorTarget : TargetRules
{
	public GardeFrontiereEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("GardeFrontiere");
	}
}

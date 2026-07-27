using UnrealBuildTool;

public class GardeFrontiereTarget : TargetRules
{
	public GardeFrontiereTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("GardeFrontiere");
	}
}

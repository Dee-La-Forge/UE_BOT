using UnrealBuildTool;

public class GardeFrontiereTarget : TargetRules
{
	public GardeFrontiereTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		// V6 : defaut d'UE 5.7 — voir GardeFrontiereEditor.Target.cs.
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("GardeFrontiere");
	}
}

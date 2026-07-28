using UnrealBuildTool;

public class GardeFrontiereEditorTarget : TargetRules
{
	public GardeFrontiereEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		// V6 est le defaut d'UE 5.7. Avec V5, le target divergeait de
		// UnrealEditor sur UndefinedIdentifierWarningLevel — divergence
		// refusee tant que les deux partagent leurs produits de build.
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("GardeFrontiere");
	}
}

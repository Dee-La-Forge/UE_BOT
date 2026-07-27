// Module runtime du dispositif Garde Frontiere.

using UnrealBuildTool;

public class GardeFrontiere : ModuleRules
{
	public GardeFrontiere(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"UMG",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Pont vers le sidecar IA (ws://127.0.0.1:8765)
			"WebSockets",
			"Json",
			"JsonUtilities",

			// Reception des blendshapes faciaux
			"LiveLink",
			"LiveLinkInterface",

			// Lecture du capteur de presence sur port serie
			"SERIALCOM",

			// Lecture audio de la parole de l'agent
			"AudioMixer",
		});
	}
}

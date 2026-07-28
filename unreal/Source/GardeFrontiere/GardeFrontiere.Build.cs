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

			// Convai n'est conserve que pour ses animations MetaHuman :
			// les AnimBP de corps et de visage, et les poses d'emotion.
			// Ni visemes ni suivi du regard — le lipsync viendra
			// d'Audio2Face par LiveLink, et le LiDAR ne rend pas d'angle.
			// La dependance est declaree ici pour une raison de build :
			// ConvaiEditor la declare deja, mais UBT ne planifiait pas le
			// module runtime pour autant, et l'edition de liens echouait
			// sur UnrealEditor-Convai.lib introuvable. La declarer depuis
			// un module du projet force sa construction.
			"Convai",
		});
	}
}

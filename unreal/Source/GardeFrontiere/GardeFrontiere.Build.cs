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

			// LiveLink/LiveLinkInterface retires : declares pour la
			// reception de blendshapes NeuroSync, chemin abandonne
			// (docs/LIPSYNC-DECISION.md) — aucun include ne les utilisait.

			// Lecture du capteur de presence sur port serie
			"SERIALCOM",

			// Lecture audio de la parole de l'agent
			"AudioMixer",

			// Capture du micro du visiteur et detection d'activite vocale.
			// Le module Silero n'est PAS declare : son fournisseur est
			// instancie par nom de classe, pour que le projet compile meme
			// si le plugin est retire.
			"RuntimeAudioImporter",

			// Audio2Face-3D. On passe par FAudio2XSession, qui prend des
			// trames int16 au fil de l'eau — l'API publique, elle, veut un
			// USoundWave cuit, ce qui suppose d'enregistrer un clip avant de
			// l'animer. Voir patches/NV_ACE_Reference-UE5.7.md.
			"ACERuntime",
			"ACECore",

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

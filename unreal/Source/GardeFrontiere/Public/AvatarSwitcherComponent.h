// Rotation des avatars MetaHuman.
//
// Reprend le mecanisme releve dans SwitchPersonScene : l'acteur courant est
// DETRUIT et un nouveau est spawne depuis un tirage dans MetahumanClasses.
// Ce n'est pas de l'activation/masquage — l'ancien code detruisait bien.
//
// Une difference assumee avec l'original : le tirage evite de ressortir le
// meme visage deux fois de suite. Avec trois avatars, un tirage purement
// aleatoire le fait une fois sur trois — assez pour que deux visiteurs
// successifs le remarquent, et l'illusion du changement tombe.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AvatarSwitcherComponent.generated.h"

class UAnimationAsset;
class UAnimInstance;
class USkeletalMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnAvatarChange, AActor*, NouvelAvatar, int32, Index);

UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API UAvatarSwitcherComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAvatarSwitcherComponent();

	/** BP_AgentGermain, BP_AgentLouise, BP_AgentTrinity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Avatars")
	TArray<TSubclassOf<AActor>> ClassesAvatars;

	/** Transform de spawn — 0,0,0 / 0,0,90 / 1,1,1 dans l'original. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Avatars")
	FTransform TransformSpawn = FTransform(FRotator(0.f, 90.f, 0.f), FVector::ZeroVector);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Avatars")
	bool bEviterRepetition = true;

	/**
	 * Retire du MetaHuman les composants conversationnels de Convai.
	 *
	 * BP_AgentGermain descend de BP_ConvaiCharacterBase et herite donc d'un
	 * ConvaiChatbotComponent. Sa seule presence suffit a reveiller toute la
	 * pile Convai : le ConvaiPlayerComponent du pion joueur ouvre le micro,
	 * Silero demarre, et une session gRPC part vers les serveurs Convai —
	 * alors que l'IA est desormais locale.
	 *
	 * A l'arret, ce trio se bloque apres StopAudioStream et gele l'editeur.
	 *
	 * ConvaiFaceSync et les AnimBP Convai_MetaHuman_FaceAnim / BodyAnim ne
	 * sont pas touches : ce sont les animations, la raison pour laquelle le
	 * plugin est conserve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Avatars")
	bool bRetirerConvaiConversationnel = true;

	// -- Audio2Face -------------------------------------------------------

	/**
	 * Prepare l'avatar pour Audio2Face : composant ACE et AnimBP facial.
	 *
	 * Fait au spawn plutot que dans le Blueprint de l'avatar, pour la meme
	 * raison que le retrait des composants Convai : trois avatars a modifier
	 * a la main, c'est trois occasions d'en oublier un.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Avatars|Audio2Face")
	bool bPreparerAudio2Face = true;

	/**
	 * AnimBP facial recevant la pose d'Audio2Face.
	 *
	 * Face_AnimBP vient du projet d'exemple Kairos de NVIDIA : c'est leur
	 * montage de reference, avec le noeud ApplyACEAnimation et la pose de
	 * correspondance ARKit. Il remplace Convai_MetaHuman_FaceAnim, qui ne
	 * pilotait plus rien depuis le retrait du chatbot.
	 *
	 * Vider ce champ laisse l'AnimBP d'origine en place.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Avatars|Audio2Face")
	TSoftClassPtr<UAnimInstance> AnimBPFacial =
		TSoftClassPtr<UAnimInstance>(FSoftObjectPath(
			TEXT("/Game/MetaHumans/Common/Face/Face_AnimBP.Face_AnimBP_C")));

	UPROPERTY(BlueprintReadOnly, Category = "Avatars")
	TObjectPtr<AActor> AvatarCourant;

	UPROPERTY(BlueprintReadOnly, Category = "Avatars")
	int32 IndexCourant = -1;

	UPROPERTY(BlueprintAssignable, Category = "Avatars")
	FOnAvatarChange OnAvatarChange;

	/** Detruit l'avatar courant et en spawne un autre. */
	UFUNCTION(BlueprintCallable, Category = "Avatars")
	AActor* Permuter();

	/** Spawne un avatar precis — mise au point et demonstration. */
	UFUNCTION(BlueprintCallable, Category = "Avatars")
	AActor* Spawner(int32 Index);

	/** Maillage facial de l'avatar courant, pour le composant d'expression. */
	UFUNCTION(BlueprintPure, Category = "Avatars")
	USkeletalMeshComponent* TrouverMaillageFacial() const;

	/** Maillage du corps — celui que le visage et les grooms suivent. */
	UFUNCTION(BlueprintPure, Category = "Avatars")
	USkeletalMeshComponent* TrouverMaillageCorps() const;

	/**
	 * Impose une animation au corps de l'avatar courant.
	 *
	 * Nullptr rend la main a l'AnimBP du MetaHuman, plutot que de laisser
	 * l'agent fige sur la derniere pose jouee.
	 */
	UFUNCTION(BlueprintCallable, Category = "Avatars")
	void JouerAnimationCorps(UAnimationAsset* Animation, bool bBoucler = true);

protected:
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;

private:
	int32 Tirer() const;
	void DetruireCourant();

	/** Detruit les composants Convai conversationnels ; rend le nombre retire. */
	int32 RetirerConvaiConversationnel(AActor* Avatar) const;

	/** Pose le composant ACE et l'AnimBP facial sur l'avatar. */
	void PreparerAudio2Face(AActor* Avatar) const;

	mutable int32 IndexPrecedent = -1;
};

#include "SidecarClient.h"

#include "GardeFrontiere.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void USidecarClient::Connecter(const FString& Url)
{
	// Une tentative EN COURS compte comme occupee. La minuterie de
	// reconnexion rappelle Connecter toutes les ~5 s, alors qu'un echec TCP
	// peut prendre plus longtemps : remplacer le socket en plein handshake
	// laissait l'ancien vivant dans le WebSocketsManager, delegues attaches
	// — double connexion s'il aboutissait, acces a un objet detruit sinon.
	// On laisse la tentative se resoudre : elle finira par SurConnexion ou
	// SurErreur, qui relanceront la mecanique.
	if (Socket.IsValid() && (Socket->IsConnected() || bConnexionEnCours))
	{
		return;
	}

	// Reliquat d'une tentative passee : on le detache proprement avant d'en
	// creer un autre, sans quoi ses delegues resteraient lies a this.
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		Socket->OnRawMessage().Clear();
		Socket.Reset();
	}

	// Un message binaire coupe en plein vol par la deconnexion precedente
	// laisserait un demi-fragment : la premiere trame de la nouvelle
	// connexion s'y concatenerait, et un reliquat impair decalerait tout le
	// PCM16 d'un octet — du bruit a la place de la voix.
	FragmentEnCours.Reset();

	// Meme raison pour ces deux-la : une trame annoncee mais jamais remise,
	// ou une cloture en attente d'elle, ne doivent pas survivre a la
	// connexion qui les a produites.
	bTrameAudioAttendue = false;
	bFinDiffere = false;

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		FModuleManager::Get().LoadModule(TEXT("WebSockets"));
	}

	Socket = FWebSocketsModule::Get().CreateWebSocket(Url);

	Socket->OnConnected().AddUObject(this, &USidecarClient::SurConnexion);
	Socket->OnConnectionError().AddUObject(this, &USidecarClient::SurErreur);
	Socket->OnClosed().AddUObject(this, &USidecarClient::SurFermeture);
	Socket->OnMessage().AddUObject(this, &USidecarClient::TraiterMessage);
	Socket->OnRawMessage().AddLambda(
		[this](const void* Donnees, SIZE_T Taille, SIZE_T Restant)
		{
			// Unreal livre les messages binaires par FRAGMENTS. Restant > 0
			// annonce une suite. Traiter chaque fragment comme une trame
			// complete etait faux a deux titres : une frontiere tombant sur
			// un octet impair perdait une demi-valeur et decalait tout le
			// reste d'un octet — du bruit a la place de la voix — et chaque
			// fragment declenchait son propre tour de traitement.
			//
			// Nos trames TTS atteignent 16 000 octets : la fragmentation
			// n'est pas une hypothese.
			FragmentEnCours.Append(static_cast<const uint8*>(Donnees), Taille);

			if (Restant > 0)
			{
				return;   // le message n'est pas fini
			}

			// L'implementation WebSocket d'Unreal presente AUSSI les
			// messages TEXTE a OnRawMessage, APRES les avoir passes a
			// OnMessage. Un descripteur parole.audio armait donc la
			// reception... puis se faisait consommer LUI-MEME comme trame
			// audio, a la place de la vraie qui suivait.
			//
			// Le symptome etait deroutant : « l'agent prend la parole » au
			// journal, et pas un son — QueueAudio recevait du JSON. Il s'en
			// plaignait d'ailleurs (ensure « BufferSize % SampleByteSize »,
			// le texte ayant un nombre impair d'octets), et chaque vidage
			// de pile coutait 2,8 s au thread de jeu.
			//
			// On ecarte donc explicitement ce qui est du JSON de notre
			// protocole — il commence par '{' et finit par '}' — avant
			// meme de consulter le descripteur.
			const bool bEstJson =
				FragmentEnCours.Num() >= 2
				&& FragmentEnCours[0] == '{'
				&& FragmentEnCours.Last() == '}';

			if (!bEstJson && bTrameAudioAttendue)
			{
				bTrameAudioAttendue = false;
				TraiterBinaire(FragmentEnCours);

				// La trame attendue est remise : la cloture qui patientait
				// peut partir. C'est ICI qu'elle doit le faire, apres
				// TraiterBinaire — l'ordre est tout l'objet du correctif.
				if (bFinDiffere)
				{
					bFinDiffere = false;
					OnParoleFin.Broadcast();
				}
			}
			FragmentEnCours.Reset();
		});

	UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : connexion a %s"), *Url);
	bConnexionEnCours = true;
	Socket->Connect();
}

void USidecarClient::Deconnecter()
{
	if (!Socket.IsValid())
	{
		return;
	}

	// Detacher AVANT de fermer. Sans cela, la fermeture declenche OnClosed,
	// qui diffuse OnPanne — vers un acteur potentiellement en cours de
	// destruction. C'est la source la plus probable d'un gel a l'arret.
	Socket->OnConnected().Clear();
	Socket->OnConnectionError().Clear();
	Socket->OnClosed().Clear();
	Socket->OnMessage().Clear();
	Socket->OnRawMessage().Clear();

	UE_LOG(LogGardeFrontiere, Warning, TEXT("  Sidecar : delegues detaches"));

	// Close() aussi sur une tentative en cours : il l'annule, la ou ne rien
	// faire laissait une connexion zombie aboutir cote serveur.
	if (Socket->IsConnected() || bConnexionEnCours)
	{
		Socket->Close();
		UE_LOG(LogGardeFrontiere, Warning, TEXT("  Sidecar : Close() rendu"));
	}

	// On lache la reference sans attendre la poignee de fermeture : le
	// sidecar est un processus distinct, rien ne garantit qu'il reponde
	// dans le delai d'un arret de PIE.
	Socket.Reset();
	bConnexionEnCours = false;
	FragmentEnCours.Reset();
	UE_LOG(LogGardeFrontiere, Warning, TEXT("  Sidecar : reference liberee"));
}

bool USidecarClient::EstConnecte() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void USidecarClient::BeginDestroy()
{
	Deconnecter();
	Super::BeginDestroy();
}

// -- Emissions -----------------------------------------------------------

void USidecarClient::EnvoyerEvenement(const FString& Nom)
{
	if (!EstConnecte())
	{
		// Pas de connexion : on previent plutot que d'echouer en silence.
		// La borne doit rester parlante, meme sans IA.
		OnPanne.Broadcast(FString::Printf(
			TEXT("sidecar injoignable (evenement %s ignore)"), *Nom));
		return;
	}
	Socket->Send(FString::Printf(TEXT("{\"evenement\":\"%s\"}"), *Nom));
}

void USidecarClient::SignalerPresence()      { EnvoyerEvenement(TEXT("presence.detectee")); }
void USidecarClient::SignalerAbsence()       { EnvoyerEvenement(TEXT("presence.perdue")); }
void USidecarClient::SignalerSilence()       { EnvoyerEvenement(TEXT("visiteur.silencieux")); }
void USidecarClient::ReinitialiserSession()  { EnvoyerEvenement(TEXT("session.reset")); }

void USidecarClient::EnvoyerAudioVisiteur(const TArray<uint8>& PCM16)
{
	if (!EstConnecte() || PCM16.Num() == 0)
	{
		return;
	}
	Socket->Send(PCM16.GetData(), PCM16.Num(), /*bIsBinary=*/true);
}

// -- Reception -----------------------------------------------------------

void USidecarClient::TraiterMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Lecteur = TJsonReaderFactory<>::Create(Message);

	if (!FJsonSerializer::Deserialize(Lecteur, Json) || !Json.IsValid())
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Sidecar : message illisible (%d octets)"), Message.Len());
		return;
	}

	const FString Evenement = Json->GetStringField(TEXT("evenement"));

	if (Evenement == TEXT("session.demarree"))
	{
		const FString Avatar = Json->HasField(TEXT("avatar"))
			? Json->GetStringField(TEXT("avatar")) : TEXT("");
		UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : session demarree (%s)"), *Avatar);
	}
	else if (Evenement == TEXT("parole.debut"))
	{
		const FString Texte = Json->HasField(TEXT("texte"))
			? Json->GetStringField(TEXT("texte")) : FString();
		const FString Emo = Json->HasField(TEXT("emotion"))
			? Json->GetStringField(TEXT("emotion")) : TEXT("Neutral");
		OnParoleDebut.Broadcast(Texte, VersEmotion(Emo));
	}
	else if (Evenement == TEXT("parole.audio"))
	{
		// Ce descripteur precede la trame binaire : on retient son taux, et
		// on ARME la reception — c'est lui, et lui seul, qui autorise la
		// prochaine trame a partir dans le chemin audio.
		if (Json->HasTypedField<EJson::Number>(TEXT("taux")))
		{
			TauxAudioAttendu = Json->GetIntegerField(TEXT("taux"));
		}
		bTrameAudioAttendue = true;
	}
	else if (Evenement == TEXT("parole.fin"))
	{
		// Une trame a ete annoncee mais pas encore remise : la cloture
		// attend. La diffuser maintenant ferait retomber bRepliqueEnCours
		// avant l'arrivee de l'audio, qui serait alors jete — voir
		// bFinDiffere.
		if (bTrameAudioAttendue)
		{
			bFinDiffere = true;
			return;
		}
		OnParoleFin.Broadcast();
	}
	else if (Evenement == TEXT("emotion"))
	{
		OnEmotion.Broadcast(VersEmotion(Json->GetStringField(TEXT("valeur"))));
	}
	else if (Evenement == TEXT("verdict"))
	{
		const FString Decision = Json->GetStringField(TEXT("decision"));
		const EGuardVerdict D = VersVerdict(Decision);

		// Un verdict illisible passait pour EnCours, que l'affichage binaire
		// du tampon transformait en REFUSE : une retouche cote Python (casse,
		// accent) refusait tous les visiteurs sans une ligne d'erreur.
		if (D == EGuardVerdict::EnCours)
		{
			UE_LOG(LogGardeFrontiere, Warning,
				TEXT("Sidecar : verdict illisible '%s' — ignore"), *Decision);
			return;
		}

		UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : verdict %s"),
			D == EGuardVerdict::Accepte ? TEXT("ACCEPTE") : TEXT("REFUSE"));
		OnVerdict.Broadcast(D);
	}
	else if (Evenement == TEXT("session.terminee"))
	{
		OnSessionTerminee.Broadcast();
	}
	else if (Evenement == TEXT("erreur"))
	{
		const FString Code = Json->HasField(TEXT("code"))
			? Json->GetStringField(TEXT("code")) : TEXT("inconnue");
		UE_LOG(LogGardeFrontiere, Error, TEXT("Sidecar : erreur %s"), *Code);
		OnPanne.Broadcast(Code);
	}
	else
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Sidecar : evenement inconnu '%s'"), *Evenement);
	}
}

void USidecarClient::TraiterBinaire(const TArray<uint8>& Donnees)
{
	if (Donnees.Num() > 0)
	{
		OnAudioRecu.Broadcast(Donnees, TauxAudioAttendu);
	}
}

// -- Cycle de vie de la connexion ----------------------------------------

void USidecarClient::SurConnexion()
{
	bConnexionEnCours = false;
	UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : connecte"));
}

void USidecarClient::SurErreur(const FString& Erreur)
{
	bConnexionEnCours = false;
	UE_LOG(LogGardeFrontiere, Error, TEXT("Sidecar : echec de connexion — %s"), *Erreur);
	OnPanne.Broadcast(Erreur);
}

void USidecarClient::SurFermeture(int32 Code, const FString& Raison, bool bParPair)
{
	bConnexionEnCours = false;
	UE_LOG(LogGardeFrontiere, Warning,
		TEXT("Sidecar : deconnecte (code %d, %s)"), Code, *Raison);

	// La trame qu'on differait n'arrivera plus : on clot avant d'annoncer
	// la panne. Sans cela, bRepliqueEnCours restait verrouille a vrai —
	// micro sourd — pour une trame que la socket morte ne remettra jamais.
	// C'est exactement le trou que bFinDiffere pourrait creuser s'il n'etait
	// purge que du cote heureux.
	if (bFinDiffere)
	{
		bFinDiffere = false;
		bTrameAudioAttendue = false;
		OnParoleFin.Broadcast();
	}

	OnPanne.Broadcast(Raison.IsEmpty() ? TEXT("connexion fermee") : Raison);
}

// -- Conversions ---------------------------------------------------------

EGuardEmotion USidecarClient::VersEmotion(const FString& Valeur)
{
	if (Valeur == TEXT("Stare"))     return EGuardEmotion::Stare;
	if (Valeur == TEXT("Concerned")) return EGuardEmotion::Concerned;
	if (Valeur == TEXT("Angry"))     return EGuardEmotion::Angry;
	if (Valeur == TEXT("Happy"))     return EGuardEmotion::Happy;
	return EGuardEmotion::Neutral;
}

EGuardVerdict USidecarClient::VersVerdict(const FString& Valeur)
{
	// Sans casse : la grammaire GBNF garantit ACCEPTE/REFUSE en majuscules,
	// mais le verdict traverse deux depots — une retouche Python ne doit pas
	// pouvoir refuser tous les visiteurs en silence.
	if (Valeur.Equals(TEXT("ACCEPTE"), ESearchCase::IgnoreCase)) return EGuardVerdict::Accepte;
	if (Valeur.Equals(TEXT("REFUSE"), ESearchCase::IgnoreCase))  return EGuardVerdict::Refuse;
	return EGuardVerdict::EnCours;
}

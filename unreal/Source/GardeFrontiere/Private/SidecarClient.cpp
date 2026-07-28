#include "SidecarClient.h"

#include "GardeFrontiere.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void USidecarClient::Connecter(const FString& Url)
{
	if (Socket.IsValid() && Socket->IsConnected())
	{
		return;
	}

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
		[this](const void* Donnees, SIZE_T Taille, SIZE_T /*Restant*/)
		{
			TArray<uint8> Tampon;
			Tampon.Append(static_cast<const uint8*>(Donnees), Taille);
			TraiterBinaire(Tampon);
		});

	UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : connexion a %s"), *Url);
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

	if (Socket->IsConnected())
	{
		Socket->Close();
		UE_LOG(LogGardeFrontiere, Warning, TEXT("  Sidecar : Close() rendu"));
	}

	// On lache la reference sans attendre la poignee de fermeture : le
	// sidecar est un processus distinct, rien ne garantit qu'il reponde
	// dans le delai d'un arret de PIE.
	Socket.Reset();
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
		// Ce descripteur precede la trame binaire : on retient son taux et
		// sa frise de visemes, pour les diffuser AVEC l'audio qu'ils
		// decrivent. Les separer les desynchroniserait au premier hoquet.
		if (Json->HasTypedField<EJson::Number>(TEXT("taux")))
		{
			TauxAudioAttendu = Json->GetIntegerField(TEXT("taux"));
		}

		VisemesAttendus.Reset();

		const TArray<TSharedPtr<FJsonValue>>* Frise = nullptr;
		if (Json->TryGetArrayField(TEXT("visemes"), Frise) && Frise)
		{
			VisemesAttendus.Reserve(Frise->Num());

			for (const TSharedPtr<FJsonValue>& Element : *Frise)
			{
				const TSharedPtr<FJsonObject>* Objet = nullptr;
				if (!Element.IsValid() || !Element->TryGetObject(Objet) || !Objet)
				{
					continue;
				}

				FString Pose;
				double Debut = 0.0;
				double Fin = 0.0;

				if (!(*Objet)->TryGetStringField(TEXT("pose"), Pose) || Pose.IsEmpty())
				{
					continue;
				}
				(*Objet)->TryGetNumberField(TEXT("debut"), Debut);
				(*Objet)->TryGetNumberField(TEXT("fin"), Fin);

				// Une pose de duree nulle ou negative ne s'ouvrirait jamais.
				if (Fin <= Debut)
				{
					continue;
				}

				FGuardViseme V;
				V.Pose = FName(*Pose);
				V.Debut = static_cast<float>(Debut);
				V.Fin = static_cast<float>(Fin);
				VisemesAttendus.Add(V);
			}
		}
	}
	else if (Evenement == TEXT("parole.fin"))
	{
		OnParoleFin.Broadcast();
	}
	else if (Evenement == TEXT("emotion"))
	{
		OnEmotion.Broadcast(VersEmotion(Json->GetStringField(TEXT("valeur"))));
	}
	else if (Evenement == TEXT("verdict"))
	{
		const EGuardVerdict D = VersVerdict(Json->GetStringField(TEXT("decision")));
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
		OnAudioRecu.Broadcast(Donnees, TauxAudioAttendu, VisemesAttendus);
	}

	// Consommee ou non, la frise ne vaut que pour la trame qu'elle annonce.
	// La garder ferait rejouer les memes poses sur l'audio suivant si un
	// descripteur arrivait sans visemes.
	VisemesAttendus.Reset();
}

// -- Cycle de vie de la connexion ----------------------------------------

void USidecarClient::SurConnexion()
{
	UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : connecte"));
}

void USidecarClient::SurErreur(const FString& Erreur)
{
	UE_LOG(LogGardeFrontiere, Error, TEXT("Sidecar : echec de connexion — %s"), *Erreur);
	OnPanne.Broadcast(Erreur);
}

void USidecarClient::SurFermeture(int32 Code, const FString& Raison, bool bParPair)
{
	UE_LOG(LogGardeFrontiere, Warning,
		TEXT("Sidecar : deconnecte (code %d, %s)"), Code, *Raison);
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
	if (Valeur == TEXT("ACCEPTE")) return EGuardVerdict::Accepte;
	if (Valeur == TEXT("REFUSE"))  return EGuardVerdict::Refuse;
	return EGuardVerdict::EnCours;
}

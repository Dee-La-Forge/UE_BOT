# Narrative Design Convai — export du 27/07/2026

Releve depuis le dashboard Convai (compte `abo-frontieres`),
Character ID `080151a8-4885-11f1-a397-42010a7be02e`.

**Cette matiere n'existait nulle part dans le depot.** Elle vivait
uniquement sur les serveurs Convai. C'est le document de reference du
scenario ; `sidecar/scenario/agent.yaml` en est la transcription
operationnelle.

## Graphe

```
        INTRO  (trigger)
          │
          ▼
    Interrogatoire  (section)
          │
     ┌────┴────┐
     ▼         ▼
   refus    accepte      (sections terminales)
```

## INTRO — trigger

**Trigger ID** : `829bc40c-ceb4-11f0-b666-42010a7be027`
**Destination** : `Interrogatoire`

> Tu interpelles le visiteur par une formule breve et froide.
> Tu te presentes uniquement comme garde-frontiere.

## Interrogatoire — section

**Objective**

> You conduct a border control interview strictly following these rules:
> - Ask ONE question at a time. Wait for a full answer before continuing.
> - Ask between 5 and 10 questions. Never fewer than 5.
> - Do NOT deliver a verdict or transition before 5 questions are answered.

**Decision 1** → `accepte`

> The border guard has just delivered his final verdict sentence granting
> access to the visitor.

**Decision 2** → `refus`

> The border guard has just delivered his final verdict sentence denying
> access to the visitor.

## accepte — section terminale

**Section ID** : `9a67d152-200d-11f1-b5df-42010a7be02c`

> The interview is complete.
> Close the conversation with a cold, brief, administrative sentence.
> You grant the visitor access to the territory.
> You do not congratulate them. You do not smile.

## refus — section terminale

**Section ID** : `8836d87a-200d-11f1-b145-42010a7be02c`

> The interview is complete.
> Close the conversation with a cold, cutting sentence.
> You deny the visitor access to the territory.
> You do not explain your decision in detail.

---

## Ce que l'export confirme

**Le chemin du verdict est etabli.** Les deux Section IDs ci-dessus
apparaissent **textuellement** dans les chaines de
`BP_ConvaiCharacterBase.uasset`. Le Blueprint compare donc le
`NarrativeSectionID` recu a ces deux UUID codes en dur, et en deduit quel
tampon afficher via `WBP_Stamp`.

En local, ce mecanisme est remplace par le tag `[VERDICT:ACCEPTE|REFUSE]`
contraint par grammaire GBNF — meme resultat, mais deterministe et sans
UUID en dur.

## Ce que l'export revele

**1. La phase de sortie n'est PAS dans Convai.**
Le graphe s'arrete a `accepte` / `refus`. Toute l'etape « quittez la zone de
detection pour liberer la place » — `IsExitStampPhase`, `SwitchToExitStamp`,
`Exit_Stamp`, `AbsenceCount`, `ResetLidarState` — est geree **cote Unreal**,
dans le Blueprint. Elle n'a donc jamais dependu du cloud, et n'est pas
concernee par la migration.

**2. Le nombre de questions : 5 a 10, jamais moins de 5.**
Mon placeholder en prevoyait 3. Correction integree.

**3. Melange de langues assume.**
Le trigger INTRO est redige en francais, les objectives des sections en
anglais. Convai s'en accommodait ; **un LLM local y sera plus sensible**.
Consigne de sortie explicite a prevoir dans le prompt systeme.

## ⚠️ Ce qui manque encore

Le Narrative Design donne le **deroule**, pas le **personnage**. Restent a
exporter, depuis les autres onglets du dashboard Convai :

- **Backstory / personnalite** de l'agent (onglet Character) ;
- **Knowledge bank** eventuelle ;
- **reglages de voix** (pour choisir une voix Piper/XTTS approchante) ;
- le detail complet des objectives — les panneaux etaient **defilants**,
  du texte peut subsister sous la zone visible.

## 📌 Observation pour la refonte — debit de visiteurs

5 a 10 questions, a ~1,5 s de latence par tour plus le temps de parole de
part et d'autre, place une session entre **2 et 4 minutes**. Dans un musee
avec file d'attente, c'est long, et le dispositif ne sert qu'un visiteur a
la fois.

Ce n'est pas un defaut — c'est un choix de conception, deja en production.
Mais si le debit devient un sujet, le levier le plus direct est le plancher
de 5 questions, pas la latence technique.

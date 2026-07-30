"""Nettoyage du texte avant synthese vocale.

Un LLM produit reguliairement des formes qui se lisent bien a l'ecran mais
s'entendent mal : ecriture inclusive, markdown, emoji, guillemets typo.
Le TTS les prononce litteralement — "signale parenthese e parenthese".

Le prompt seul ne suffit pas : un modele 3B y retombe. On filtre donc en
sortie, de facon deterministe.

Sans dependance lourde : testable en une seconde.
"""

from __future__ import annotations

import re

# --- Ecriture inclusive -------------------------------------------------
# signale(e) -> signale     ·  cher(e)s -> chers  ·  etudiant(e)(s) -> etudiants
_PARENTHESE_INCLUSIVE = re.compile(r"\(([eEs]{1,2})\)", re.UNICODE)
# point median : cher·es -> cher  ·  visiteur·euses -> visiteur
# Le suffixe est pris en entier (jusqu'a la fin de la suite de lettres) :
# le borner a 3 caracteres laissait tramer la fin de "·euses".
_POINT_MEDIAN = re.compile(r"[·•]\s?[a-zA-Zàâäéèêëïîôöùûüÿç]+", re.UNICODE)

# --- Markdown -----------------------------------------------------------
_GRAS_ITALIQUE = re.compile(r"(\*{1,3}|_{1,3})(.+?)\1", re.DOTALL)
_TITRE = re.compile(r"^\s{0,3}#{1,6}\s*", re.MULTILINE)
_CODE = re.compile(r"`{1,3}([^`]*)`{1,3}")
_LIEN = re.compile(r"\[([^\]]+)\]\([^)]*\)")
_PUCE = re.compile(r"^\s*[-*+]\s+", re.MULTILINE)

# --- Divers -------------------------------------------------------------
_EMOJI = re.compile(
    "[\U0001F000-\U0001FAFF\U00002600-\U000027BF\U0001F1E6-\U0001F1FF️]+"
)

# Filet de securite : le modele glisse parfois le nom de l'emotion en tete
# de replique ("Concerned." ou "Stare : Papiers."). Ce sont des etiquettes
# de controle, pas des paroles — le TTS les prononcerait en anglais.
# Le \b est indispensable : sans lui, "Neutralite exigee" devenait
# "ite exigee".
_ETIQUETTE_EMOTION = re.compile(
    r"^\s*(Stare|Concerned|Angry|Neutral|Happy)\b\s*[:.\-–]?\s*",
    re.IGNORECASE,
)
# --- Appellatifs de genre -----------------------------------------------
#
# La persona l'interdit en toutes lettres : « Tu ne designes JAMAIS le genre
# du visiteur : ni "monsieur", ni "madame". » Le modele passe outre des que
# le visiteur donne un prenom marque — releve par bench/dialogue_test.py le
# 31/07/2026, sur deux profils de visiteur sur trois :
#
#   « vous mentez, madame. Qui etes-vous vraiment ici ? »
#   « Doutez-vous de vos mots, monsieur Victor ? »
#
# La grammaire GBNF ne peut rien ici : elle contraint la FORME d'une
# replique — deux phrases, terminaison en « ? » — jamais son VOCABULAIRE.
# Un plancher de caracteres n'interdit pas un mot.
#
# Deux passes, parce que la virgule ne se traite pas pareil selon la suite :
#
#   « monsieur Victor »           -> « Victor »        (l'appellatif seul)
#   « de vos mots, monsieur ? »   -> « de vos mots ? » (avec sa virgule)
#
# Sans la premiere, « de vos mots, monsieur Victor ? » perdait sa virgule et
# donnait « de vos mots Victor ? ».
_APPELLATIFS = r"monsieur|madame|mademoiselle|messieurs|mesdames|m'sieur|m'dame"
# Suivi d'un nom propre : on ne retire que l'appellatif.
_APPELLATIF_DEVANT_NOM = re.compile(
    rf"\b(?:{_APPELLATIFS})\s+(?=[A-ZÀ-Ý])", re.IGNORECASE)
# Seul : il emporte la virgule qui le precede.
_APPELLATIF_SEUL = re.compile(
    rf"[,;]?\s*\b(?:{_APPELLATIFS})\b", re.IGNORECASE)

_ESPACES = re.compile(r"[ \t]{2,}")
_SAUTS = re.compile(r"\n{2,}")

# Les guillemets francais s'accompagnent d'espaces insecables internes
# (« non ») : on les absorbe avec le guillemet, sinon il reste " non ".
_GUILLEMET_OUVRANT = re.compile("«[\\s  ]*")
_GUILLEMET_FERMANT = re.compile("[\\s  ]*»")

_REMPLACEMENTS = {
    "…": "...",
    "“": '"', "”": '"',
    "’": "'", "‘": "'",
    "–": "-", "—": "-",
    " ": " ",   # espace insecable
    " ": " ",   # espace fine insecable
}


def nettoyer_pour_tts(texte: str) -> str:
    """Rend un texte prononcable.

    >>> nettoyer_pour_tts("Avez-vous ete signale(e) ?")
    'Avez-vous ete signale ?'
    >>> nettoyer_pour_tts("**Papiers**, s'il vous plait.")
    "Papiers, s'il vous plait."
    >>> nettoyer_pour_tts("Doutez-vous de vos mots, monsieur Victor ?")
    'Doutez-vous de vos mots, Victor ?'
    >>> nettoyer_pour_tts("Vous mentez, madame. Qui etes-vous ?")
    'Vous mentez. Qui etes-vous ?'
    >>> nettoyer_pour_tts("Madame, avancez.")
    'Avancez.'
    """
    if not texte:
        return ""

    t = texte

    # Markdown d'abord : ses delimiteurs gênent les autres motifs.
    t = _LIEN.sub(r"\1", t)
    t = _CODE.sub(r"\1", t)
    t = _GRAS_ITALIQUE.sub(r"\2", t)
    t = _TITRE.sub("", t)
    t = _PUCE.sub("", t)

    # Ecriture inclusive.
    t = _PARENTHESE_INCLUSIVE.sub("", t)
    t = _POINT_MEDIAN.sub("", t)

    t = _EMOJI.sub("", t)
    t = _ETIQUETTE_EMOTION.sub("", t)

    # Appellatifs de genre : l'ordre des deux passes compte, voir plus haut.
    t = _APPELLATIF_DEVANT_NOM.sub("", t)
    t = _APPELLATIF_SEUL.sub("", t)

    # Guillemets francais avant la table : ils emportent leurs espaces.
    t = _GUILLEMET_OUVRANT.sub('"', t)
    t = _GUILLEMET_FERMANT.sub('"', t)

    for avant, apres in _REMPLACEMENTS.items():
        t = t.replace(avant, apres)

    t = _SAUTS.sub("\n", t)
    t = _ESPACES.sub(" ", t)

    # Espace parasite devant la ponctuation faible (", " et ". ").
    t = re.sub(r"\s+([,.!?;:])", r"\1", t)
    # ... mais le francais garde l'espace avant ! ? ; :
    t = re.sub(r"([!?;:])", r" \1", t)
    t = _ESPACES.sub(" ", t)

    # Un appellatif retire en TETE de replique (« Madame, avancez. ») laisse
    # sa ponctuation orpheline. On la coupe, et on rend la majuscule que le
    # mot supprime portait.
    t = t.strip()
    t = re.sub(r"^[,;:.\s]+", "", t)
    if t and t[0].islower():
        t = t[0].upper() + t[1:]

    return t.strip()

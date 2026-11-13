# Mission : module Cmajor transient shaper, avec preuve de rendu

Contexte frais. Tu travailles dans /home/randomaccessmemories/Documents/GIT/OpenVST.
Lis d'abord AGENTS.md, docs/MODULE_FORMAT.md, spikes/runtime/modules/stereo_gain.cmajor,
spikes/runtime/modules/stereo_gain.cmajorpatch, spikes/runtime/modules/note_grid.cmajor,
spikes/runtime/modules/note_grid.cmajorpatch, spikes/runtime/harness.cpp,
spikes/runtime/build.sh et tests/runtime/. Dépôt public :
https://github.com/arthurkowskii/openvst

Objectif : écrire un vrai module de transient shaper en Cmajor, utilisable dans le
conteneur, avec des tests qui prouvent son comportement sur des signaux construits.

Exigences DSP :
- Effet stéréo, `audioIn` et `audioOut` en `float32<2>`, plus des paramètres
  d'événements float32 : attack, sustain et gain de sortie au minimum.
- Deux enveloppes de détection distinctes, rapide et lente, sur le signal d'entrée.
  Le gain appliqué dépend du rapport entre les deux : transitoire accentué quand
  l'enveloppe rapide dépasse la lente, corps réduit sinon. Formule simple,
  documentée en commentaire, pas de magie.
- Bornes explicites : gains en dB convertis en linéaire, clampés, pas de NaN, pas
  d'infini, pas de gain négatif. Attaque et sustain doivent être neutres à 0 dB.
- Aucune allocation, aucune lecture disque, traitement échantillon par échantillon
  ou par bloc, état interne minimal et déterministe.

Fichiers à produire, et seulement ceux-là :
- spikes/runtime/modules/transient_shaper.cmajor
- spikes/runtime/modules/transient_shaper.cmajorpatch (même format que les
  fixtures existantes, avec un ID de module propre, par exemple
  org.openvst.spike.transient-shaper, version 0.0.1)
- tests/runtime/test_transient.py : tests exécutés, pas de simulation Python du
  DSP. Tu passes par le harness Cmajor existant, comme tests/runtime/run_dsp.py.

Tests attendus, tous exécutés :
1. Signal constant : la sortie reste dans une tolérance serrée de l'entrée quand
   attack et sustain sont à 0 dB.
2. Impulsion isolée : le pic transitoire est renforcé quand attack > 0.
3. Train de décroissance : le corps est réduit quand sustain < 0, sans faire
   passer la sortie sous zéro ni produire de NaN.
4. Stéréo : les deux canaux sont traités indépendamment, avec des entrées
   différentes, sans fuite d'un canal sur l'autre.
5. Paramètres hors bornes envoyés par le test (au-delà du max) : le module borne
   au lieu de partir en vrille. Si le harness refuse les valeurs hors bornes,
   dis-le et teste la borne exacte.
6. Découpage en blocs irréguliers : le résultat ne dépend pas de la taille de bloc.

Preuves :
- `sh spikes/runtime/build.sh` puis tes tests, avec les sorties réelles recopiées.
- Hash SHA-256 final de transient_shaper.cmajor et de son .cmajorpatch, à mettre
  dans le rapport : le conteneur les vérifie.
- artifacts/runtime/TRANSIENT_REPORT.md : mesures obtenues (valeurs numériques),
  limites du détecteur, ce que ton test ne prouve pas, et la confirmation que tu
  n'as pas touché aux fichiers hors de ta liste.

Interdits : modifier src/plugin/, tests/packages/, les fixtures existantes, le
harness, la config Hermes. Aucun commit ni push. Pas de sudo, pas d'installation.

Budget 40 tours. Si le moteur refuse une construction Cmajor après trois essais,
rapporte le message exact au lieu de contourner par un DSP simulé en Python.

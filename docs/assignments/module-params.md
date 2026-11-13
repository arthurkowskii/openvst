# Mission : paramètres par module dans le conteneur, et branchement du transient shaper

Contexte frais. Dépôt : /home/randomaccessmemories/Documents/GIT/OpenVST, public
https://github.com/arthurkowskii/openvst. Lis AGENTS.md, README.md,
docs/MODULE_FORMAT.md, src/plugin/ModuleRegistry.h, src/plugin/Processor.cpp,
src/plugin/Processor.h, src/plugin/TrustedDSP.h, src/plugin/NativeEditor.h,
tests/plugin/ProcessorTests.cpp, puis spikes/runtime/modules/transient_shaper.cmajor
et artifacts/runtime/TRANSIENT_REPORT.md.

État réel : le conteneur sait choisir un module par identité, mais n'expose qu'un
seul paramètre, `openvst.fx.gain.v1`, envoyé à l'endpoint `gain`. Un module à trois
paramètres comme le transient shaper verrait `attack` et `sustain` figés à 0 dB et
se comporterait comme un simple gain. C'est la raison pour laquelle il n'est pas
encore dans le registre.

Objectif : un modèle de paramètres par module, générique et statique, puis
l'enregistrement du transient shaper comme second effet sélectionnable.

Spécification attendue :
1. Chaque entrée de `ModuleInfo` déclare sa liste de paramètres : id de paramètre
   stable, nom affiché ASCII, endpoint Cmajor, min, max, défaut, unité optionnelle.
   Le DSP lit cette table, plus de cas particulier pour `gain`.
2. Identifiants d'automation permanents. `openvst.fx.gain.v1` doit rester
   strictement identique pour le module `stereo-gain` : un projet existant garde
   son automation. Pour les paramètres du transient shaper, utilise des ID
   préfixés par le module, par exemple `openvst.ts.attack.v1`,
   `openvst.ts.sustain.v1`, `openvst.ts.gain.v1`.
3. Les paramètres doivent être visibles du DAW de façon stable. JUCE ne permet pas
   d'ajouter ou retirer des paramètres à chaud : déclare donc l'union statique des
   paramètres de tous les modules du registre, dans un ordre figé, et n'envoie un
   événement au DSP que pour ceux du module actif. Un paramètre inactif reste
   présent et sans effet, il ne disparaît pas.
4. `getStateInformation` sauvegarde les valeurs des paramètres du module actif, en
   plus de l'identité du module. Un état ancien, qui ne contient que `gain`, doit
   toujours se restaurer : les paramètres absents prennent leur défaut. Un état
   dont une valeur sort de la plage du module est refusé, pas corrigé en silence.
5. `TrustedDSP.cpp/.h` : à la préparation, résoudre chaque endpoint déclaré et
   échouer explicitement si un paramètre du module n'existe pas dans le patch, ou
   si le patch en déclare un que la table ignore. Toujours aucune allocation ni
   aucun accès disque dans `processBlock`, toujours aucune attente du worker.
6. Enregistrer le transient shaper dans `ModuleRegistry.h` avec ses deux hashs
   exacts, relevés dans artifacts/runtime/TRANSIENT_REPORT.md, et son processeur
   principal `TransientShaper`. Il doit apparaître dans le combo de l'éditeur FX.
7. L'éditeur natif affiche un curseur par paramètre du module actif, avec son nom
   et sa valeur. Pas de graphisme, pas de HTML.

Contraintes :
- Ne touche pas à spikes/runtime/modules/* (le module est figé, hash vérifié),
  ni à tests/packages/, ni à tests/runtime/.
- Aucun commit, aucun push, aucune installation système, pas de sudo.
- Ne casse pas les tests existants : les cinq blocs nommés du registre et les
  contrôles historiques (hash mismatch, cross-variant, truncation) doivent rester
  verts.

Attendus :
- Tests natifs ajoutés : un état ancien à un seul gain restaure le module par
  défaut avec ses défauts de paramètres ; changer de module expose les bons
  paramètres ; une valeur hors plage est refusée ; les paramètres inactifs sont
  sans effet ; l'automation de `openvst.fx.gain.v1` produit toujours le même
  rendu qu'avant sur `stereo-gain`.
- Build et suite réellement exécutés, sorties recopiées.
- Rapport artifacts/plugin/PARAMS_REPORT.md : décisions, nombre de paramètres
  exposés, ce qui reste faux, et confirmation que le rendu de `stereo-gain` est
  inchangé (compare un rendu avant/après si tu peux le faire dans un test).

Budget 45 tours. Si un point bloque deux fois, arrête et rapporte le message exact
plutôt que de contourner par une solution qui casse l'automation existante.

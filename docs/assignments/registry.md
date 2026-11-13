# Mission : registre de modules dans le conteneur OpenVST

Contexte frais. Tu travailles dans /home/randomaccessmemories/Documents/GIT/OpenVST.
Lis d'abord AGENTS.md, README.md, docs/MODULE_FORMAT.md, puis src/plugin/Processor.cpp,
src/plugin/Processor.h, src/plugin/TrustedDSP.h, src/plugin/NativeEditor.h et
tests/plugin/ProcessorTests.cpp. Le dépôt public correspondant est
https://github.com/arthurkowskii/openvst

Problème réel : le build FX charge toujours en dur la fixture `stereo_gain`, et le
build Instrument toujours `note_grid`. Impossible d'ajouter un deuxième effet sans
casser les projets existants, parce qu'il n'existe aucune notion de module
sélectionné.

Objectif de cette tranche : introduire une sélection de module par identité stable,
sans casser les projets existants ni l'automation.

Contraintes fortes :
- Le comportement actuel doit rester identique par défaut. Un projet existant qui
  ne contient aucun choix explicite doit retrouver `stereo_gain` (FX) et
  `note_grid` (Instrument), mêmes paramètres, même état, mêmes hash.
- Table de modules déclarative et statique (pas de scan de dossier, pas de
  chargement dynamique arbitraire) : id de module, nom affiché, chemin de source
  et de manifeste, hash SHA-256 attendu, type (effect/instrument), nom du
  processeur Cmajor. Les deux fixtures actuelles y entrent.
- La sélection passe par l'état du plugin : champ versionné, validé, borné. Un id
  inconnu est refusé explicitement, jamais remplacé silencieusement. Un état
  ancien (sans champ de module) doit être accepté et interprété comme le module
  par défaut.
- Aucun paramètre exposé au DAW ne doit changer d'identifiant. Le paramètre
  `openvst.fx.gain.v1` garde son ID et sa plage. Si un module n'a pas de gain, le
  paramètre doit rester présent mais neutre plutôt que disparaître, sinon
  l'automation existante casse. Dis clairement dans ton rapport ce que tu as
  choisi et pourquoi.
- Rien de neuf dans le thread audio : sélection lue avant préparation, résolue
  dans prepareToPlay, compilation toujours hors callback, pas d'allocation dans
  processBlock.
- L'éditeur natif doit afficher le module actif (id et hash), et permettre de
  changer de module via un menu déroulant simple, sans casser le timer existant.

Attendus :
1. Refactor borné : une petite abstraction de registre, pas une réécriture de
   Processor.cpp. Fichiers nouveaux autorisés sous src/plugin/.
2. Tests natifs ajoutés dans tests/plugin : état sans champ module retombe sur le
   défaut, état avec id inconnu refusé, état avec id connu charge le bon module,
   roundtrip d'état avec module, paramètre de gain toujours présent et identique.
3. Build et tests exécutés réellement, résultats bruts dans ton rapport.
4. Rapport bref dans artifacts/plugin/REGISTRY_REPORT.md : décisions, limites,
   ce qui reste faux.

Interdits : modifier spikes/runtime/modules/*, ne pas toucher aux tests packages,
aucun commit ni push, aucun changement de config Hermes, aucune installation
système, pas de sudo, ne pas supprimer build/ ni research/.

Budget 40 tours. Si le build échoue trois fois sur un point, arrête et rapporte le
blocage exact plutôt que d'inventer un contournement.

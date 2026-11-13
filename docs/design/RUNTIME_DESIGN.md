# OpenVST : conception du runtime

## Statut et preuves

Conception uniquement. Autorité : `docs/PROJECT_BRIEF.md:5-60`. Un module actif par instance, aucun VST tiers ; Discover, My Library, Publish et Unapproved restent intégrés au plugin.

Checkout inspecté : `research/cmajor`, SHA `17d08e3fd2a8b5d0bdee40abe572a646e32b89e2`, confirmé par `git rev-parse HEAD`. `git submodule status` confirme les sous-modules non initialisés. Aucun build ni test audio exécuté. Les références ci-dessous sont relatives à ce checkout et à ce SHA. Les annonces web transmises par le parent ne constituent pas des validations locales. Tous les budgets numériques proposés restent à mesurer.

## 1. Décision

| Option | Adéquation et risques | Maintenance / décision |
|---|---|---|
| Réutiliser le lecteur sans changement | JIT et wrappers disponibles, mais rechargement automatique, slots réaffectés par ordre, état orienté chemin | Faible au départ, exigences non satisfaites. Non retenu en production |
| Forker le lecteur | Bon témoin de faisabilité ; modifier sécurité, état, automation, UI et installation touche ses fondations | Élevée, conflits upstream récurrents. Réservé au spike comparatif |
| Conteneur minimal, API Cmajor intégrée | Contrat hôte maîtrisé, réemploi moteur et helpers sélectionnés ; isolation à construire | Coût initial supérieur, frontière upstream plus étroite. Recommandation conditionnelle |
| Autre runtime | Aucun blocage définitif de Cmajor démontré | Pas de remplacement décidé ; réouvrir seulement après no-go mesuré |

Preuves : `tools/CmajPlugin/Source/cmaj_PatchLoaderPlugin.cpp:113-131` crée Patch, active l'auto-rebuild et choisit SinglePatchJITPlugin/JITLoaderPlugin. `include/cmajor/helpers/cmaj_JUCEPlugin.h:1200-1218,1231-1239` expose le chargeur ; `:945-952` réaffecte les paramètres par ordre et peut agrandir leur pool. Ce n'est pas notre contrat d'immutabilité.

Cmajor reste candidat. JUCE reste candidat pour les wrappers, pas une décision de licence acquise. Ne pas développer un conteneur complet avant comparaison bornée avec le lecteur.

## 2. Frontières et sécurité

Architecture visée :

- Audio hôte : adaptation préallouée, files bornées, aucune compilation, attente bloquante, allocation, opération disque ou réseau. Échange du moteur à une frontière de bloc ; destruction différée.
- Compilation : service séparé, entrées immuables, quotas mémoire/CPU/temps, accès seulement au paquet, cache non exécutable fourni par l'auteur interdit. Parser et compilateur sont aussi des surfaces hostiles.
- DSP communautaire natif : processus dédié par instance pour le premier essai public, permissions OS minimales, sans réseau, sans secrets, ressources préouvertes en lecture seule. IPC validé, mémoire partagée limitée aux buffers nécessaires.
- UI de confiance : shell OpenVST, bibliothèque et catalogue ; jetons de publication confinés au courtier réseau. Aucun secret transmis au module.
- UI communautaire : d'abord contrôles générés depuis un schéma déclaratif. JS et patch workers communautaires désactivés initialement. Leur activation exige une isolation distincte, navigation et réseau interdits, pont limité aux paramètres et messages typés, sans accès aux fichiers ni à Publish.
- Catalogue : téléchargement, intégrité/origine, extraction défensive, installation atomique par hash ; serveur distinct de l'exécution DSP. Soumission privée, validation isolée obligatoire, Unapproved opt-in avec badge textuel, revue liée à la version exacte. Signature et revue ne prouvent pas l'innocuité.

Le backend LLVM charge du code natif et des symboles externes (`modules/compiler/src/backends/LLVM/cmaj_LLVMPerformer.cpp:244-265,310-321,339-364`). Cela ne prouve pas un accès arbitraire depuis chaque source, mais impose de refuser les fonctions externes non autorisées. L'API bas niveau exige des types corrects, sinon comportement indéfini (`include/cmajor/API/cmaj_Performer.h:83-118`). Valider l'adaptateur hors audio.

Distinguer quatre propriétés : sandbox mémoire contre corruption ; permissions OS contre accès aux ressources ; processus séparé contre propagation des crashes ; échéance et abandon contre boucles infinies. Aucune n'implique les autres. Ni JIT ni processus séparé ne suffisent. Le contrôle statique de certaines boucles (`modules/compiler/src/validation/cmaj_Validator.h:1880-1904`) n'est pas un budget d'exécution.

Le natif direct minimise copies et synchronisation, mais une faute ou boucle peut immobiliser le DAW. L'IPC pipeliné ajoute une latence déclarée ; le thread audio ne doit jamais attendre le worker. Échéance manquée : silence instrument, signal sec retardé effet, diagnostic et redémarrage hors audio. Une isolation OS réellement restrictive reste à démontrer sous Windows/macOS. Un backend à mémoire isolée avec interruption bornée serait préférable si performant ; l'implémentation WebAssembly lue utilise une WebView (`modules/compiler/src/backends/WebAssembly/cmaj_WebAssemblyPerformer.cpp:19-30,38-68`), pas une preuve de VM audio adaptée.

Conclusion : le runtime examiné ne peut pas être déclaré sûr pour du code hostile. Ouverture publique bloquée tant que confinement, dépassements et latence ne sont pas éprouvés. Le catalogue principal ne bénéficie pas d'une exemption de sécurité.

## 3. Contrat hôte et automation

Choix : 128 slots normalisés fixes, identifiants permanents `ovst.slot.000` à `ovst.slot.127`, réservés dès construction. Capacité proposée, pas limite validée. Table persistée slot → UUID paramètre, domaine, courbe et version du schéma. Jamais d'association par ordre de manifeste ; jamais de changement silencieux de sens ou d'échelle.

Changer de module ne transfère pas l'automation : avertissement systématique, anciennes liaisons conservées comme tombstones, nouveaux paramètres sur slots libres après confirmation. Le plugin ne peut pas déduire fiablement l'existence d'enveloppes dans tous les DAW. Si épuisement, refuser l'exposition supplémentaire et proposer une nouvelle instance. Ne pas recycler automatiquement les slots. Les libellés hôte peuvent rester génériques ; paramètres dynamiques et rafraîchissement universels non promis.

Rappel : restaurer la table exacte avant l'état du module. Migration explicite, versionnée et réversible, sur copie ; garder l'ancien état. Tester écritures gestuelles, lecture, undo et collisions à l'ouverture. Le lecteur existant réserve initialement 100 paramètres mais peut en ajouter (`include/cmajor/helpers/cmaj_JUCEPlugin.h:1205-1210,945-952`) : comportement à remplacer.

MIDI : conserver offsets échantillon et ordre, y compris aux bornes ; subdiviser les blocs autour des événements si nécessaire. L'entrée bas niveau n'accepte pas d'offset dans `addInputEvent` (`include/cmajor/API/cmaj_Performer.h:104-120`), alors que le wrapper transmet `samplePosition` (`include/cmajor/helpers/cmaj_JUCEPlugin.h:293-319`). Vérifier le trajet complet, pas seulement sa signature. Transport : tempo, PPQ, signature, seeks, boucles et arrêt ; valeurs absentes explicitement marquées.

PDC : latence DSP plus pipeline, fixe durant lecture ; reconfiguration négociée à l'arrêt. Le wrapper remonte une latence (`include/cmajor/helpers/cmaj_JUCEPlugin.h:443-452`) mais annonce une tail nulle (`:153`) : insuffisant pour délais/réverbérations. Déclarer tails finies ou inconnues et tester leur rendu. Bypass effet avec dry retardé et transition ; instrument muet avec extinction des notes. Reset après stop/seek selon politique documentée, sans exiger la conservation des buffers transitoires.

Binaires initiaux 64 bits. Audio float32 initial ; refuser proprement le mode float64 ou convertir avec buffers préalloués, sans annoncer du DSP double précision. Le wrapper double actuel assert (`:322`). Offline sans UI ni réseau, préparation terminée avant rendu, échec explicite si moteur absent. Le worker offline doit synchroniser chaque quantum avec un timeout borné adapté au rendu, sans appliquer les échéances murales temps réel ni produire du silence pour continuer artificiellement. Aucun bloc perdu acceptable en export. En temps réel, utiliser un quantum IPC fixe indépendant des tailles de bloc hôte variables, buffers préalloués et latence en échantillons déclarée ; dimensionnement à mesurer. Tester blocs variables, minuscules et nuls, changements de fréquence, surcharge MIDI et fermetures de fenêtre.

## 4. État et conservation

État hôte versionné : UUID module, version immuable, hash cryptographique du paquet et des ressources, moteur exact avec SHA/runtime ABI/backend/options, table de slots, paramètres, état persistant déclaré et checksum. Séparer cet état logique de la RAM DSP transitoire ; ne pas promettre un snapshot de voix ou de tail arbitraire.

Stockage adressé par contenu, versions côte à côte, sources/manifeste conservés. Cache natif local invalidable par architecture/runtime, jamais seule copie restaurable. Petites ressources exportables avec le projet ; banques volumineuses référencées par hashes avec outil de collecte, inventaire et avertissement explicite de non-portabilité. Quotas, droits de redistribution et sauvegarde restent à fixer.

Conserver les anciens moteurs identifiés dans des workers séparés et archives reproductibles ; distinguer reproductibilité d'installation et identité sonore inter-CPU, non acquise. Aucun remplacement automatique par le dernier moteur. Ancien moteur vulnérable : suspendre son exécution, pas effacer l'état.

Module absent : instance placeholder conservant état et automation, récupération du hash exact dans My Library hors audio, import local vérifié si hors ligne ; jamais substitution approximative. Révocation : bloquer nouveaux téléchargements, afficher motif/version, préserver sources, projet et état ; quarantaine explicite de l'exécution dangereuse avec parcours de récupération. Pas de suppression silencieuse. La suppression utilisateur doit tenir compte des projets non inventoriés.

## 5. Variantes, plateformes et licences

Deux identités plugin permanentes : effet stéréo entrée/sortie ; instrument MIDI vers sortie stéréo, adaptation mono explicite pour SineSynth. Pas de changement effet/instrument d'une instance. VST3 Windows/macOS d'abord ; AU macOS à valider séparément. Linux sert aux tests moteur, pas à valider ces plateformes. Le lecteur déclare AU/VST3/Standalone et MIDI (`tools/CmajPlugin/CMakeLists.txt:26-36`), sans démonstration locale de compatibilité. Son build active LLVM (`:71-80`).

| Élément effectivement lu | Texte exact / portée |
|---|---|
| Licence racine Cmajor | GPLv3-or-later ou commerciale, fichiers marqués Cmajor ; tiers exclus : `LICENSE.md:3-9` |
| Lecteur, helper JUCE, backends LLVM/WebAssembly | En-têtes GPLv3 ou commercial ; helper `include/cmajor/helpers/cmaj_JUCEPlugin.h:4-17`, lecteur `tools/CmajPlugin/Source/cmaj_PatchLoaderPlugin.cpp:4-12`, WebAssembly `modules/compiler/src/backends/WebAssembly/cmaj_WebAssemblyPerformer.cpp:4-17` ; licence racine pour portée « ou ultérieure » |
| StereoDelay | GPLv3 ou commercial : `examples/patches/StereoDelay/StereoDelay.cmajor:4-12` |
| SineSynth | Aucun avis propre dans le source lu ; copyright par défaut annoncé dans `README.md:69`, attribution/licence à confirmer avant redistribution |
| JUCE, CHOC, LLVM, SDK formats | Licences exactes non inspectées, aucune conclusion. CHOC absent ; JUCE demandé via `CMakeLists.txt:94-99`, LLVM récupéré au tag `LLVM_20` par `3rdParty/dependencies.cmake:3-10` |

Cible open source envisageable sous voie GPL compatible, non décision juridique finale. Inventaire transitif, licences commerciales éventuelles et obligations de distribution bloquent le choix final. Le C++ généré a une exception décrite dans `LICENSE.md:11`, qui ne détermine pas la licence des modules ni des samples. Plateforme, serveur, modules et ressources auront leurs licences séparées.

## 6. Spikes bornés proposés, non exécutés

Prérequis : copie de travail dédiée, sous-modules initialisés, dépendances/licences figées, compilateurs et SDK, JUCE local identifié. Linux nécessite GTK/WebKit (`tools/command/CMakeLists.txt:53-57`). Ne pas lancer ici les téléchargements/builds.

Depuis la racine upstream, préparation future :

```sh
git submodule update --init --recursive
cmake -S . -B "$SPIKE_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_PLUGIN=ON -DBUILD_EXAMPLES=OFF -DBUILD_CMAJ_LIB=OFF -DJUCE_PATH="$JUCE_PATH"
cmake --build "$SPIKE_BUILD" --target cmaj CmajPlugin_VST3 --config Release --parallel 2
```

Options/cible source : `CMakeLists.txt:23-26,91-100`, `tools/command/CMakeLists.txt:26`, `tools/CmajPlugin/CMakeLists.txt:26-32`. Générateurs Xcode/Visual Studio documentés dans `README.md:35-64` ; cibles générées à confirmer sur chaque OS. Budget proposé : une journée par configuration ; arrêt sur dépendance non reproductible, sans dériver vers une reconstruction LLVM illimitée.

Avec `CMAJ` pointant vers le binaire construit et fixtures WAV/MIDI préparées dans un espace dédié :

```sh
"$CMAJ" render --input="$FIXTURES/stereo.wav" --output="$OUT/delay.wav" --blockSize=32 examples/patches/StereoDelay/StereoDelay.cmajorpatch
"$CMAJ" render --midi="$FIXTURES/poly.mid" --length=48000 --rate=48000 --channels=2 --blockSize=32 --output="$OUT/synth.wav" examples/patches/SineSynth/SineSynth.cmajorpatch
```

Syntaxe : `tools/command/Source/cmaj_command_Render.h:30-68` ; création du chemin output à confirmer, helper CHOC absent. Stéréo prouvée par `examples/patches/StereoDelay/StereoDelay.cmajor:23-24` ; allocation polyphonique par `examples/patches/SineSynth/SineSynth.cmajor:8-28`. Aucun rendu obtenu ici.

Budget proposé : deux jours moteur, deux jours par OS dans Reaper. Comparer lecteur et adaptateur minimal : buffers 16/32/64/128/512, fréquences 44,1/48/96 kHz, blocs irréguliers, voix saturées. Mesurer CPU médian/p99/max callback, xruns, allocations, RAM, compilation froide/chaude, PDC à l'impulsion, jitter IPC, exactitude MIDI, tails, offline et réouverture hors ligne/UI fermée. Go : fixtures correctes, aucune attente/allocation audio observée, aucun dépassement sur campagne définie, marge CPU convenue ; sinon no-go ciblé, pas verdict universel.

Budget sécurité séparé proposé : trois jours de tests adversariaux bornés, archives malformées, compilations excessives, boucles dépendantes des données, crashes et messages IPC invalides. Go public uniquement après confinement OS et reprise démontrés, menace résiduelle acceptée et audit complémentaire. Une campagne passée ne prouve pas l'absence de vulnérabilités.

## 7. Bloquants

Isolation native et UI non démontrée ; contrat IPC/latence non mesuré ; aucun build Windows/macOS/Reaper ; licences transitives absentes ; conservation des moteurs et banques non financée ; quotas, authentification, révocation signée et protocole serveur à spécifier. Sécurité du backend à mémoire isolée, packaging/signature/notarisation et comportement des hôtes restent à vérifier.

Résumé : privilégier un conteneur minimal avec runtime Cmajor conditionnel, éprouvé contre le lecteur existant. Ne pas ouvrir l'exécution communautaire avant les gates de sécurité, automation, restauration et performances. Aucun audit de conception ne permet une promesse « irréprochable ».

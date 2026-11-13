# Conception OpenVST : travaux lancés

Statut : conception initiale livrée, lue et réconciliée par le parent dans `../design/ARCHITECTURE.md`. Runtime terminé normalement ; catalogue/QA arrêté par limite de 25 tours (exit 1), après écriture des deux livrables. Le parent a corrigé JSON, séparation slots/paramètres, frontière UI, atomicité et reprise d'installation, révocation, règles d'accès et gates QA. Présence des cinq documents et syntaxe du bloc JSON vérifiées par Python (exit 0). Aucun produit implémenté, build ou test audio exécuté. Prochaine étape : preuves techniques privées et inventaire des dépendances, pas déploiement public.

## Répartition

- Runtime : `gpt-6.1-sol`, provider `openai-codex`, effort `medium`, processus `proc_89472803c855`. Brief `runtime-design.md`, log `runtime-run.log`, livrable attendu `../design/RUNTIME_DESIGN.md`.
- Catalogue et QA : `deepseek-v4.1-flash`, provider `opencode-go`, effort `max`, processus `proc_fe3ab34f584f`. Brief `catalogue-design.md`, log `catalogue-run.log`, livrables attendus `../design/CATALOGUE_DESIGN.md` et `../design/QUALITY_GATES.md`.

Lancements CLI isolés car delegate_task ne permet pas d'épingler modèle/effort par tâche dans son schéma courant. Aucun réglage global modifié. Max 900 secondes et nombre de tours borné par processus, notifications de fin activées. Ces limites sont des budgets de travail, pas une échéance promise.

Le parent doit lire les livrables réels, vérifier les affirmations de code upstream, résoudre les contradictions et produire la synthèse avant d'annoncer une conception terminée. Si un lancement échoue ou un modèle est indisponible, le signaler, ne pas substituer silencieusement un autre modèle.

## Sources

Clone de recherche Cmajor : `../../research/cmajor`, commit `17d08e3fd2a8b5d0bdee40abe572a646e32b89e2`. Sous-modules non initialisés ; aucune compilation. Pas encore de dépôt Git OpenVST ou de remote.

Le parent a vérifié `tools/CmajPlugin/CMakeLists.txt` : JUCE, formats AU/VST3/Standalone, entrées/sorties MIDI et performer LLVM sont explicitement déclarés. Cela ne prouve pas le comportement dans un DAW ni la sûreté de modules hostiles. `LICENSE.md` précise GPLv3-or-later/commercial et la responsabilité de vérifier les licences tierces.

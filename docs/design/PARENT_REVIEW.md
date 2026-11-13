# Revue parent : runtime, première passe

Statut : revue partielle du document RUNTIME_DESIGN.md. Le retour catalogue/QA doit être réconcilié avant synthèse finale. Aucun build ni test audio exécuté.

## Vérifications directes

Au checkout Cmajor 17d08e3fd2a8b5d0bdee40abe572a646e32b89e2, vérification des points structurants :

- tools/CmajPlugin/Source/cmaj_PatchLoaderPlugin.cpp:113-131 : auto-rebuild activé, sélection du lecteur JIT.
- include/cmajor/helpers/cmaj_JUCEPlugin.h:925-963 : le lecteur multi-patch réaffecte les slots par ordre et agrandit le pool ; le chemin fixed-patch dispose, lui, d'un rebind par ID. Ne pas généraliser la fragilité à tous les modes Cmajor.
- include/cmajor/helpers/cmaj_JUCEPlugin.h:153 : tail annoncée nulle. Comportement à corriger et tester pour OpenVST, sans prétendre que chaque DAW coupe nécessairement la sortie.

Ces lectures soutiennent une intégration sélective plutôt qu'un simple rebranding du lecteur, sans constituer un verdict de performances.

## Ajustements requis avant plan exécutable

1. Offline et IPC : distinguer le callback temps réel (ne jamais attendre le worker) du rendu offline (synchronisation hors contrainte temps réel, timeout borné et erreur explicite). Appliquer une échéance murale temps réel à un export accéléré causerait du silence ou des données perdues. Aucune adaptation automatique de la qualité ni drop silencieux n'est acceptable en export.
2. Pipeline : définir une taille de quantum fixe indépendante de la taille variable des blocs hôte, ses buffers préalloués et son délai déclaré ; vérifier blocs nuls, changements de taille et compensation PDC. Le nombre de blocs n'est pas une unité de latence stable si leurs tailles changent.
3. Automation : 128 slots est une proposition de budget à mesurer. Le modèle tombstone protège les anciennes associations mais consomme les slots au fil des changements de module. Définir une UX avertissant la capacité restante et une nouvelle instance comme voie sûre. Ne pas prétendre que le plugin peut effacer ou détecter les enveloppes de tous les hôtes. Paramètres supplémentaires non exposés doivent être distingués de paramètres manquants.
4. Compatibilité : préserver les identités, sources et options de moteur n'assure pas une restauration bit-identique entre CPU/OS. Spécifier tolérances et fixtures de régression ; distinguer absence de perte de données, compatibilité et déterminisme numérique.
5. Sécurité : processus dédié et permissions minimales constituent une architecture proposée. Les mécanismes Windows/macOS exacts, handles hérités, chargement des dépendances et droits de compilation JIT restent des gates bloquantes, pas une protection déjà disponible.
6. Interfaces : les contrôles déclaratifs proposés pour les premiers modules communautaires limitent le graphisme HTML/JS prévu. Distinguer le shell de confiance HTML/CSS éventuel, les modules locaux de développement et les modules communautaires ; ne pas faire passer cette restriction de sécurité proposée pour une exigence déjà approuvée par le mainteneur.

## Conclusion provisoire

RUNTIME_DESIGN.md est une base de conception utile, pas encore une architecture approuvée ni implémentée. Les sujets critiques sont l'isolation de code communautaire, le contrat automation/recall et le coût de latence du worker. Aucun changement de moteur n'est décidé à ce stade.

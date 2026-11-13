# OpenVST : architecture de référence et décisions

## Portée et niveau de preuve

Synthèse parent des propositions de GPT 6.1 SOL (medium) et DeepSeek V4.1 Flash (max), après première revue et corrections. Référence produit : docs/PROJECT_BRIEF.md. Il s'agit d'une conception initiale permettant de borner les prototypes, pas d'une architecture certifiée ni d'un produit implémenté.

Le runtime est fondé sur une inspection des sources Cmajor au SHA 17d08e3fd2a8b5d0bdee40abe572a646e32b89e2. Aucun build, rendu, test DAW ou test de sécurité exécuté. Le travail DeepSeek a atteint sa limite de tours après écriture des deux documents ; le parent les a lus, corrigés et réconciliés au lieu de déclarer le run réussi ou de le relancer entièrement.

## Architecture retenue pour les prototypes

```text
DAW
 ├─ OpenVST FX : identité fixe, entrée/sortie audio
 └─ OpenVST Instrument : identité fixe, MIDI vers audio
      ├─ shell de confiance : Discover / My Library / Publish / Unapproved
      ├─ paramètres hôte et état du projet, indépendants de la fenêtre
      ├─ gestionnaire local : paquets immuables, versions, journal de reprise
      ├─ courtier réseau : catalogue, téléchargement, soumission, comptes
      └─ adaptateur DSP : moteur sélectionné après essais
           ├─ compilation hors audio et confinement à vérifier
           └─ exécution isolée du code tiers à démontrer

Serveur du mainteneur
 ├─ API et métadonnées du catalogue, comptes et modération
 ├─ stockage des paquets et attestations détachées
 └─ validation hostile isolée, sans accès aux secrets ni à la base
```

Deux variantes d'un même produit, pas des plugins autonomes pour chaque création. Les téléchargements et la publication ne requièrent aucune page web externe. Une connexion sert à accéder au serveur, jamais à autoriser la lecture d'un module déjà installé. Le catalogue distant fournit des données ; le shell livré avec OpenVST possède seul les privilèges d'installation et de compte.

## Décisions et raisons

1. **Cmajor reste le premier moteur à éprouver.** Comparer son lecteur existant à un conteneur minimal utilisant son API. L'inspection favorise l'intégration sélective : le chargeur multi-patch réemploie les paramètres par ordre et son wrapper annonce une tail nulle. Ne pas réécrire le DSP, mais ne pas distribuer le lecteur simplement renommé. JUCE et la voie GPL compatible restent conditionnés à l'inventaire des dépendances.
2. **Automation sous contrôle du conteneur.** Banque fixe de slots hôte, proposée à 128, et table persistée vers les identités sémantiques des paramètres. Ce nombre n'est pas gelé avant essai UX/hôte. Ne jamais lier par ordre de manifeste ni recycler implicitement une ancienne liaison. Un changement de module avertit et garde les tombstones ; une nouvelle instance est la voie sûre en cas de capacité insuffisante. La plateforme ne prétend pas savoir si le DAW possède des enveloppes.
3. **Restauration exacte des données.** Sauver ID, version, hash, moteur et options, table d'automation et état. Coexistence des versions, pas de mise à jour silencieuse. Module manquant : placeholder conservant les données. Ne pas promettre une identité numérique inter-CPU ; définir des tolérances mesurées. Les banques volumineuses nécessitent un outil de collecte et une politique de droits/rétention.
4. **Paquets immuables et installation récupérable.** SHA-256 de tout le ZIP final, attestations externes. Installation sur le même volume avec staging, vérification et marqueur avant publication atomique ; journal pour réconcilier filesystem et index après crash. Verrous entre instances hors audio, aucun dossier partiel chargeable. Les exemples sont fictifs, pas des paquets livrés.
5. **Publication par version et hash.** Quarantaine privée, tests obligatoires, Unapproved masqué par défaut, puis approbation humaine. Le contrôle d'accès vaut aussi pour les URLs directes, pas seulement pour l'affichage. Un rejet retire la distribution publique. Une révocation connue bloque l'exécution normale tout en conservant état et automation ; pas de bouton ordinaire contournant la protection.
6. **Sécurité : frontière bloquante avant code tiers.** JIT, CSP, WebView séparée et processus enfant ne suffisent pas chacun à garantir le confinement. Tester permissions OS, handles, compilation, IPC, crash et boucle infinie sur Windows/macOS. L'architecture worker doit borner les effets d'une panne sans attente du thread audio ; la latence et le quantum IPC restent à mesurer. Offline : synchronisation adaptée et erreur explicite, jamais drop silencieux ou silence de timeout.
7. **Interfaces personnalisées : objectif conservé, frontière ouverte.** HTML/CSS/JS pour un module n'est pas encore autorisé en mode communautaire. Les premiers essais peuvent employer des contrôles déclaratifs pour isoler la preuve DSP ; cela ne remplace pas définitivement les interfaces propres aux modules demandées. Leur ouverture exige une isolation distincte, sans secrets, réseau libre ni API native générale. Ce point doit être tranché avant gel du format public.
8. **Serveur minimal.** API unique, base relationnelle, stockage adressé par contenu et validation isolée. Stack et quotas proposés, pas encore sélectionnés. Authentification de publication dans le plugin, clé publique de confiance livrée au client, rotation et rollback à spécifier. Pas de paiement, racks ou éditeur IA ajoutés au périmètre.

## Ordre de validation

### P0 : preuves techniques privées, aucun code tiers

Inventorier les licences et dépendances ; compiler le lecteur témoin ; rendre un effet stéréo et un synthé polyphonique de confiance. Conserver commandes, SHA, environnement, fichiers et résultats. Une vérification Linux ne valide pas Windows/macOS.

### P1 : contrat DAW et exécution

Comparer lecteur témoin et adaptateur minimal, tester automation, rappel, fenêtre fermée, MIDI horodaté, transport, tails, bypass, PDC et offline sur chaque OS cible. Mesurer callback p99/max, échéances, xruns et latence du worker, pas seulement CPU global. Produire une décision go/no-go par mécanisme avant gel du runtime.

### P2 : tranche verticale intégrée

Catalogue minuscule de fixtures de confiance, directement dans le plugin : découvrir, télécharger un vrai paquet, installer, charger, sauvegarder, fermer puis rouvrir hors ligne. Pas besoin de créer dix modules pour prouver ce parcours. Tester interruption et installation simultanée par deux instances.

### P3 : contributions et ouverture

Démontrer confinement client/serveur avant exécution tierce. Puis comptes, envoi, rejet technique privé, Unapproved opt-in et approbation par hash. Avant ouverture publique : licences complètes, matrice plateformes définie, chaîne de confiance exercée, sauvegarde restaurée, anti-abus, confidentialité, règles de modération et validation explicite du mainteneur.

## Décisions encore ouvertes

Isolation native ou backend à mémoire isolée selon performances réelles ; API des interfaces personnalisées ; capacité de slots ; minimums OS et architectures ; conservation des moteurs ; quotas samples/serveur ; licence finale et inventaire transitif ; garde des clés et stratégie de distribution signée. Aucun de ces sujets ne doit être présenté comme résolu par ce document.

## Documents associés

- RUNTIME_DESIGN.md : inspection et proposition de runtime.
- CATALOGUE_DESIGN.md : UX, paquet, installation, API et modération, corrigés par le parent.
- QUALITY_GATES.md : matrice de preuve et gates, corrigées par le parent.
- PARENT_REVIEW.md : observations de revue du runtime.

En cas d'ambiguïté, le brief produit reste autoritatif ; cette synthèse prévaut sur les propositions techniques initiales pour les arbitrages explicitement traités. La prochaine livraison technique doit être une preuve exécutée, pas une nouvelle accumulation de documents.

# Mission : conception catalogue, paquets et qualité OpenVST

Tu es le sous-agent conception produit/paquets/QA. Réponds et rédige en français, sans em dash. Tu es lancé avec deepseek-v4.1-flash / opencode-go / reasoning max. Ne délègue pas, ne change pas de modèle, ne modifie aucune configuration Hermes. Conception uniquement : pas d'implémentation du plugin ou serveur, pas de commit/push ni déploiement. Ne lis aucune donnée personnelle inutile.

Lire /home/randomaccessmemories/Documents/GIT/OpenVST/docs/PROJECT_BRIEF.md. Exigences : conteneur de modules effets/synthés, Windows/macOS et Reaper, toutes les opérations Discover/My Library/Publish/Unapproved dans le plugin sans site externe obligatoire, lecture locale hors ligne, catalogue et soumissions sur le serveur du mainteneur, contributions Unapproved opt-in après contrôle technique, approbation humaine par version exacte, objectif open source. Un module actif par instance. Pas de VST tiers, pas de marketplace payante ni réseau social à construire. Pas d'éditeur IA embarqué requis.

Tu n'as pas à choisir le moteur DSP. Cmajor est candidat ; le parent a récupéré ses sources dans research/cmajor (checkout de recherche, pas dépendance produit). Un autre architecte traite le runtime et l'isolation. Ne lis pas son document tant qu'il travaille, pas de fichiers partagés en écriture. Sources officielles déjà vérifiées par le parent : https://cmajor.dev/docs/PatchFormat (manifeste, DSP, UI, assets), https://cmajor.dev/docs/Licence (GPLv3-or-later/commercial). Pas de recherche web déléguée : concevoir à partir du brief et des sources locales, marquer les faits non vérifiés. Pas de produits ni API inventés.

Créer uniquement ces deux documents dans /home/randomaccessmemories/Documents/GIT/OpenVST/docs/design/ :
- CATALOGUE_DESIGN.md (environ 1500 mots maximum)
- QUALITY_GATES.md (environ 1000 mots maximum)

Catalogue attendu :
- Parcours intégré réaliste et états UI, y compris annulation, reprise, fermeture fenêtre/DAW, hors ligne, authentification publication sans navigateur externe, pas de compte pour lecture locale. Informations textuelles et navigation clavier, jamais couleur seule.
- Client de confiance vs UI des modules ; aucun token auteur ou privilège réseau catalogue remis au JS communautaire.
- Architecture serveur minimale maintenable, sans Kubernetes/microservices inutiles. Stockage des paquets séparé des jobs de validation hostile, quotas, anti-abus, sauvegardes, journal de modération, signalement, suppression et rôles auteur/modérateur. Ne pas déployer.
- Contrat de paquet v0 PROPOSÉ : manifeste et exemple réaliste (pas encore standard acquis), ID/version/hash, auteur, compatibilité runtime/API, params stables, assets/licences ; pas de DLL/scripts exécutables arbitraires. Éviter une auto-référence impossible du hash du paquet.
- Soumission -> quarantaine -> tests -> Unapproved -> approved/rejected et révocation, statuts attachés au hash exact ; authenticité n'est pas sécurité. Installer un paquet malveillant reste un risque runtime non résolu ici.
- Installation transactionnelle, extraction sans traversal/liens/ZIP bomb/collisions cross-platform, interruption, stockage partagé entre plusieurs instances du plugin et verrous hors thread audio. Immutable version = ne pas pouvoir remplacer le même ID/version avec de nouveaux octets.
- Contrat API minimal avec champs et erreurs, pagination stable, upload borné et idempotence, recherche/download/approval/report. Signatures, confiance et rotation/revocation de clés à spécifier sans bricoler un protocole crypto.
- Préservation des projets et ressources ; retrait public != suppression locale ; comportement lors d'une alerte sécurité explicite.
- Jalons pour un catalogue minimal intégré réel, puis soumissions publiques ; ordre et critères testables. Ne pas rebaptiser une maquette web en plugin fonctionnel.

Qualité : matrice exigences -> tests -> preuve attendue -> bloquant. DSP/MIDI/automation/recall/offline/window closed/versions, race install multi-instances, sécurité hostile, licences/ressources/auteurs, UX/accessibilité, builds et notarisation/signature à vérifier. Budgets de performances à proposer avec matériel et protocole documentés, pas de faux chiffres acquis. Distinguer tests Python/logique, harness natif et tests dans DAW sur chaque OS. Indiquer ce qu'un test n'établit pas. Gates private prototype vs alpha vs communauté publique. Risques prioritaires et décisions absentes.

Concision, précision et coût de maintenance priment sur la quantité. Termine par les deux chemins produits et les principales questions à arbitrer par le parent.

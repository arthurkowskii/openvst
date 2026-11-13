# OpenVST : périmètre validé

## Objectif

Plateforme open source pour créer avec un agent IA des effets audio temps réel et des synthétiseurs MIDI, distribués comme modules propres à OpenVST. Un conteneur permanent dans le DAW charge les modules sans reconstruction ni réinstallation du conteneur.

Cibles initiales : Windows et macOS, Reaper comme premier DAW de validation. Les variantes effet/instrument et les formats VST3/AU restent à éprouver. Un module actif par instance au départ. Pas d'hébergement de VST tiers, de racks ou de routages complexes dans le périmètre initial.

## Expérience utilisateur obligatoire

Tout le parcours de découverte, téléchargement, installation, gestion et soumission est accessible directement depuis le plugin, sans page web externe obligatoire.

- Discover : catalogue, recherche, filtres effets/instruments, fiches et démos.
- My Library : modules installés, chargement, versions, suppression avec protection des projets existants.
- Publish : import local, validation, fiche auteur et soumission au serveur.
- Unapproved : contributions en attente de revue humaine, masquées par défaut et accessibles par activation explicite.

Les modules peuvent être créés hors du plugin par un agent. L'éditeur de code et une intégration IA embarquée ne sont pas requis.

## Catalogue communautaire

Hébergement du catalogue et des paquets sur le serveur du mainteneur. Les utilisateurs peuvent soumettre leurs propres modules pour téléchargement par les autres utilisateurs.

Circuit validé : soumission privée, contrôles automatiques obligatoires, puis publication dans Unapproved si réussite, enfin revue humaine pour accès au catalogue principal. Un échec technique reste privé avec rapport à l'auteur. Un refus humain retire la version du téléchargement public.

Unapproved ne contourne pas les contrôles techniques. Avertissement d'installation et badge textuel, jamais uniquement une couleur. Exclusion des recommandations par défaut et possibilité de signalement. L'approbation est liée à une version exacte, jamais héritée automatiquement par une mise à jour.

## Exigences structurelles

- Audio, MIDI et rendu hors ligne indépendants de l'ouverture de l'interface.
- Modules installés utilisables sans connexion au serveur ni authentification obligatoire de lecture.
- Aucune mise à jour silencieuse du DSP des projets existants.
- Versions immuables installées côte à côte ; conserver l'identité exacte du module et son état dans le projet.
- Paramètres et correspondances d'automation stables, à concevoir et tester explicitement.
- Stratégie de conservation des sources et ressources nécessaires aux anciens projets à définir, incluant les limites de taille pour les banques de samples.
- Compilation, réseau et entrées/sorties disque hors du thread audio.
- Vérification d'intégrité et d'origine des paquets. Une signature n'atteste pas leur innocuité.
- Refuser initialement les bibliothèques natives et exécutables arbitraires fournis par les auteurs.
- Permissions limitées du DSP et des interfaces de modules ; séparer les privilèges réseau du catalogue de ceux des interfaces communautaires.
- Validation de code soumis dans un environnement isolé et borné en ressources ; tests automatiques et revue ne constituent pas une garantie de sécurité.
- Contrôler archives, chemins, liens, tailles, types de fichiers, compatibilité et ressources.

## Choix techniques non figés

- Cmajor est le premier candidat DSP, pas une décision acquise.
- Évaluer le lecteur VST/AU existant de Cmajor avant de développer un conteneur neuf.
- C++/JUCE et interfaces HTML/CSS/JavaScript sont des pistes à valider.
- Auditer les licences du moteur, du lecteur et des dépendances avant de choisir la licence finale d'OpenVST. Cmajor documente une option GPLv3 ou ultérieure et une option commerciale.
- Déclarer séparément les licences de la plateforme, du serveur, des modules et de leurs ressources. Ne pas déduire la licence d'un module de celle du conteneur.
- Authentification de publication, modération, quotas, stockage, révocation de sécurité et isolation du runtime restent à spécifier.

## Premier jalon : preuve de faisabilité

1. Examiner les sources, licences et possibilités d'intégration du lecteur Cmajor.
2. Faire fonctionner un effet stéréo et un synthé polyphonique chargés comme modules.
3. Mesurer le CPU et le comportement à petits buffers ; tester MIDI, automation, fenêtre fermée, rendu hors ligne et sauvegarde/réouverture.
4. Démontrer dans le plugin un catalogue minimal avec téléchargement, installation et chargement réels. Pas de site externe en substitution au parcours intégré.
5. Démontrer une soumission, son contrôle, son statut Unapproved masqué par défaut et son approbation par version avant ouverture communautaire.

Les étapes constituent des critères de sortie, pas des résultats déjà obtenus. Aucun moteur, plugin ou serveur n'est encore implémenté à la création de ce document.

## Références de départ

- Cmajor : https://cmajor.dev/
- Format : https://cmajor.dev/docs/PatchFormat
- Licence : https://cmajor.dev/docs/Licence
- Références d'expérience fournies par le mainteneur : Lunacy Audio NOVA, Spitfire LABS, Decent Sampler. Ce document ne prétend pas décrire leurs implémentations.
- Références fonctionnelles et visuelles à examiner : les onze applications HTML de Virtual Riot signalées dans un dossier local de référence. Accès et contenu non vérifiés.

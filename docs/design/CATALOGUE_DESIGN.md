# Catalogue, paquets et soumissions OpenVST

Statut : proposition, non implémentée ni mesurée. Faits Cmajor tirés de `research/cmajor` (commit 17d08e3) et des docs officielles ; non testé = [à vérifier].

## 1. Invariants

1. Catalogue = données ; UI = code local. Aucun HTML ou JS distant exécuté.
2. Lecture et usage des modules installés hors ligne sans compte ; installation hors ligne depuis un paquet local validé. Le catalogue distant et ses téléchargements nécessitent le réseau. Compte pour publier, signaler et modérer.
3. Un module actif par instance ; pas de racks ni de VST tiers.
4. Approbation, rejet, révocation liés au SHA-256 exact, jamais au nom/version seuls.
5. Versions immuables, côte à côte, jamais de mise à jour silencieuse.
6. Le client valide avant confiance ; le serveur ne fait pas confiance au contenu soumis.

## 2. Parcours intégré

| Écran | États | Contenu et actions |
|---|---|---|
| Discover | chargement, cache hors ligne daté, vide, erreur | recherche, filtres, tri ; nom, auteur déclaré, type, taille, licence, badge textuel Approuvé/Non approuvé opt-in/Révoqué |
| Fiche | compatible, runtime trop ancien, paquet non caché | description, paramètres, démo, licence ; Installer, Mettre à jour, Voir dans la bibliothèque |
| My Library | prêt, incomplet, réparer, bloqué, versions multiples | charger, remplacer, supprimer avec avertissement projets |
| Publish | brouillon, analyse, erreur, envoi, quarantaine, tests, échec privé, revue, Unapproved, approuvé, rejeté | import ZIP, contrôle local, fiche, consentement, progression, annulation, reprise, rapport |
| Unapproved | désactivé par défaut, activé, avertissement | confirmation textuelle, badge, hors recommandations, Signaler ; file modérateur motivée |

- **Annulation.** Partiel supprimé, sauf "Reprendre plus tard" : `.part`, reprise Range, hash final seul juge. Installation annulable avant le rename ; ensuite seul "Supprimer".
- **Fenêtre ou DAW fermé.** Fermer la fenêtre laisse le transfert dans le processus hôte ; s'il quitte, l'état est interrompu et le prochain lancement propose Reprendre ou Abandonner. Staging orphelin nettoyé après 24 h ; `install.json` fait foi.
- **Hors ligne.** Index en cache daté, mention "hors ligne", My Library complet, installation depuis un paquet local uniquement.
- **Authentification publication.** Formulaire natif, code à usage unique, jeton dans Keychain/DPAPI, jamais dans le projet ni le JS module ; courriel vérifié par code dans le plugin ; sessions révocables ; pas de navigateur obligatoire [à valider].
- **Accessibilité.** Tabulation, Entrée/Échap, progression annoncée, erreurs au champ, contraste, texte redimensionnable, badges textuels, jamais la couleur seule.

## 3. Frontière de confiance

- **Client de confiance** : UI catalogue, réseau, comptes, fichiers, installation. Code local, privilèges complets, aucun contenu distant exécuté.
- **UI de module** : proposition de démarrage communautaire limitée aux contrôles déclaratifs, sans JS auteur ni patch worker. Les interfaces HTML/JS personnalisées restent un objectif, conditionné à une isolation vérifiée. Une WebView séparée et une CSP ne prouvent pas une sandbox : bloquer aussi navigation, sous-ressources, frames, ouvertures de fenêtres, téléchargements et ponts natifs ; vérifier les profils et permissions OS. Le pont ne doit accepter que des messages typés bornés, jamais un PatchConnection non audité.

Aucun jeton auteur, clé, endpoint ou privilège réseau catalogue n'est exposé au JS communautaire ; les messages UI vers l'hôte forment une liste fermée, bornée, validée. Serveur HTTP local des vues [à vérifier] : propre à l'instance, chemin non devinable, sans endpoint catalogue ni credentials ; sinon scheme interne. Images distantes décodées hors UI, sans SVG actif ; démos lues par le client, jamais par la vue.

## 4. Serveur minimal

Une VM Linux, proxy TLS, service HTTP unique, base relationnelle (SQLite WAL, PostgreSQL si besoin), répertoire content-addressed en lecture seule, worker de validation séparé. Pas de Kubernetes ni de microservices.

- **Paquets** : `packages/sha256/ab/cd/<digest>`, lecture seule, `Cache-Control: immutable`, jamais exécuté.
- **Validation** : worker sur autre VM ou conteneur non privilégié, sans réseau sortant, paquet en lecture seule, limites CPU, mémoire, temps, PID ; rapport JSON, logs bornés. Ni base ni comptes visibles.
- **Base** : modules, versions (digest, statut, auteur, dates), approbations, signalements, journal append-only, clés.
- **Rôles** : auteur, modérateur (file, décisions, révocation), admin (clés, quotas) ; 2FA.
- **Quotas** : 3 soumissions en attente par compte, 512 Mo par paquet, 2 Go par compte et par jour, 20 par mois.
- **Anti-abus** : vérification courriel, rate limit par IP et compte, blocage des téléchargements anonymes massifs ; CAPTCHA si abus.
- **Sauvegardes** : dump base quotidien, snapshot des paquets, restauration testée mensuellement, journal séparé.
- **Signalement et suppression** : formulaire par fiche, catégories (sécurité, droits, contenu), suivi modérateur. Retrait public = `unlisted` ou `revoked`, jamais suppression des octets utiles ; purge des rejets après préavis.

## 5. Contrat de paquet v0 (proposé)

ZIP unique, racine `openvst.json`, `patch/`, `assets/` ; pas de DLL, dylib, exe, script shell, binaire Mach-O ou PE ; worker JS Cmajor refusé en v0 [à vérifier].

```json
{
  "schema": "openvst.package/0",
  "module": {
    "id": "io.openvst.klang.tapesat",
    "version": "1.2.0",
    "name": "Tape Saturator",
    "kind": "effect",
    "vendor": "Atelier Klang",
    "license": "GPL-3.0-or-later"
  },
  "runtime": {
    "engine": "cmajor",
    "patch_format": 1,
    "host_api": "openvst-host/0",
    "patch": "patch/TapeSaturator.cmajorpatch"
  },
  "parameters": [
    {"index": 0, "id": "drive", "name": "Drive", "min": 0, "max": 24, "default": 6, "unit": "dB"}
  ],
  "resources": [
    {"path": "assets/ir_tape.wav", "sha256": "DIGEST_SHA256_A_CALCULER", "license": "CC0-1.0"}
  ]
}
```

- `id` reverse-DNS stable, `version` SemVer ; (id, version) immuable, tout changement de contenu = nouvelle version.
- Exemple fictif de schéma, pas paquet valide : remplacer DIGEST_SHA256_A_CALCULER par une empreinte réellement calculée avant validation.
- Hash sur tous les octets du ZIP final. Signature et attestation sont des fichiers détachés EXTERNES au ZIP, jamais une exclusion d'octets interne à calculer. Aucun hash du paquet dans openvst.json ; resources[].sha256 couvre chaque fichier annoncé.
- `.cmajorpatch` interne : même ID et version, sinon rejet.
- parameters décrit les identités sémantiques stables du module ; index est un ordinal local de schéma, pas un numéro de slot DAW global. Le conteneur possède sa table persistée slot DAW -> (module ID, paramètre ID, domaine, courbe, schéma). Jamais de réaffectation par ordre de manifeste ni de recyclage implicite. Les suppressions deviennent des tombstones, le changement de sens exige migration explicite ou nouvelle identité. Le projet conserve le hash exact, la table et les valeurs.
- `vendor` déclaratif ; l'identité faisant foi est le compte serveur. Ressource sans licence ou empreinte : rejet.
- Limites : 512 Mo, 20 000 entrées, 2 Go extraits, ratio 100:1, chemin 240 caractères.

## 6. Cycle de vie

`brouillon -> envoi -> quarantaine -> tests -> échec technique (privé, rapport auteur) | unapproved -> approved | rejected`, plus `revoked` et `withdrawn`.

Chaque transition enregistre digest, statut, acteur, date, motif, clé. Une approbation est un enregistrement signé (digest, id, version, date), pas un booléen ; nouvelle version = nouvelle quarantaine. L'authenticité n'est pas la sécurité : la signature prouve l'origine, pas l'innocuité ; installer un paquet malveillant reste un risque runtime non résolu.

## 7. Installation

1. Vérifier l'espace (2x paquet + extrait + marge), créer le staging.
2. Télécharger en `.part`, reprendre par Range, hasher en flux, borner la taille.
3. Vérifier digest, attestation, compatibilité runtime/API, licence acceptée.
4. Extraire en streaming vers `staging/<digest>.tmp` : chemin relatif normalisé NFC ; refus de `..`, absolu, lecteur, symlink, hardlink, device, FIFO ; collisions après repli de casse ; noms réservés Windows (CON, PRN, AUX, NUL, COM1..9, LPT1..9), points ou espaces finaux ; limites entrées, taille, ratio ; extensions allowlist ; fsync.
5. Écrire install.json et les empreintes dans staging avant publication, synchroniser fichiers puis répertoire selon les primitives de chaque OS. Rename atomique sur le même volume vers modules/<id>/<version>-<digest-complet>/, puis index SQLite en transaction. Le filesystem et SQLite ne forment pas une transaction unique : un journal de reprise doit réconcilier un paquet publié mais non indexé. Aucun dossier partiel n'est chargeable. Tester chaque point de crash et vérifier les empreintes, pas la seule présence du marqueur.

**Multi-instances** : advisory lock par module dans un thread de travail, jamais l'audio ; un écrivain, lecteurs sur répertoires immuables ; index WAL, l'audio ne lit ni fichier ni base. `install.json` absent = incomplet. Windows : dossier ouvert non remplaçable, nouvelle version = nouveau dossier, seul le pointeur change. Suppression : corbeille réversible, avertissement projets, jamais de purge automatique.

## 8. API minimale

- `GET /v1/modules?q=&kind=&sort=&cursor=&limit=` (public, anonyme), `GET /v1/modules/{id}`, `/versions`, `GET /v1/versions/{digest}`
- `GET /v1/packages/{digest}` (immutable, Range)
- `POST /v1/submissions` (multipart ou résumable, `Idempotency-Key`), `GET /v1/submissions/{id}` (auteur)
- `POST /v1/versions/{digest}/approve|reject|revoke` (modérateur, `If-Match`)
- `POST /v1/reports`, `POST /v1/auth/login|refresh|logout`, `GET /.well-known/openvst-keys.json`

Erreurs : `{"error":{"code":"...","message":"...","details":{},"request_id":"..."}}` ; codes `invalid_request`, `unauthorized`, `forbidden`, `not_found`, `conflict`, `payload_too_large`, `rate_limited`, `digest_mismatch`, `incompatible_runtime`, `quarantined`, `unavailable`, statut HTTP correspondant.

Pagination : curseur opaque (clé de tri + id), tri stable, `next_cursor` nul en fin. Upload borné, idempotent par (compte, clé) ; même digest = même soumission, 200 avec l'ID existant. Download : `ETag` = digest. Approbation : compare-and-set journalisé.

**Signatures** : clé Ed25519 serveur, `key_id`, attestation JSON canonique détachée, clés publiques dans le bundle et `.well-known`, rotation avec chevauchement, liste de révocation. Nouvelle installation signée par une clé révoquée refusée ; les installations existantes se chargent hors ligne. Pas de protocole maison : libsodium, formats standards.

## 9. Préservation, retrait, alerte

- Le projet enregistre id, version, digest et état. À l'ouverture, la version exacte est cherchée localement ; si absente : la télécharger, choisir un paquet local, ou ouvrir sans module en conservant l'état. Jamais de substitution par la dernière version.
- Unapproved, rejected, withdrawn et revoked ne sont pas servis par une URL publique de digest contournant leurs règles d'accès. L'opt-in Unapproved filtre aussi API, recherche et récupération directe. Un rejet cesse le téléchargement public ; les copies locales conservent leur état. La rétention serveur pour restauration est distincte du droit de servir le fichier et doit respecter droits et retraits légaux. Pas de promesse de conservation éternelle.
- Révocation connue : pas d'exécution ordinaire, même lors du rappel d'un ancien projet. Placeholder conservant automation et état ; récupération des données sans exécuter le module. Une éventuelle procédure experte d'exécution en environnement isolé est hors v0. Aucune suppression locale automatique. Hors ligne, indiquer la fraîcheur de la dernière information connue sans exiger une connexion pour les modules non révoqués connus.
- Vérification au démarrage et toutes les 24 h en ligne ; hors ligne, dernière liste connue.

## 10. Jalons

- **M1 Catalogue minimal intégré** : 5 à 10 modules curatés sur le serveur du mainteneur, recherche, fiche, téléchargement, installation, chargement dans Reaper, cache hors ligne, aucun compte. Sortie : parcours sans navigateur sur les deux OS, preuves d'installation et de chargement.
- **M2 Soumissions privées** : comptes, envoi, quarantaine, tests automatiques, Unapproved masqué, approbation et rejet par digest, journal, signalement. Sortie : soumission de bout en bout, rejet motivé, paquet hostile rejeté.
- **M3 Communauté publique** : inscription ouverte, quotas, anti-abus, sauvegardes restaurées, rotation exercée, builds signés et notarized, politique de modération publiée. Sortie : revue de sécurité et répétition d'incident.

## 11. Questions à arbitrer

- Moteur DSP et API hôte (format, validation).
- JS de vue et worker acceptés en v0 ? Accès réseau/fichier du worker [à vérifier] ?
- Signature auteur, ou compte et TLS pour v0 ?
- Rétention des octets rejetés ou retirés, disponibilité par digest.
- Qui modère, avec quel délai et quelle politique ?
- Capacité du serveur du mainteneur et licence du catalogue.
- Certificats Windows/macOS : détenteur et renouvellement ; taille des banques de samples ; Unapproved persistant ou par session ?

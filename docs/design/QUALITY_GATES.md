# Qualité et gates OpenVST

Statut : proposition, aucun test exécuté. Portée : catalogue, paquets, installation, UX et serveur ; le runtime et l'isolation relèvent de l'architecte runtime. Toute preuve cite commit, build, OS, version de DAW et fichier de session.

## 1. Couches de preuve

- **L1, logique Python** : manifeste, ZIP, hash, machine à états, extraction, client API contre un faux serveur, pagination, idempotence, quotas. CI Linux.
- **L2, harness natif** : runtime et modules, rendu audio/MIDI hors ligne, automation, recall, coupures, concurrence. Runners Windows et macOS.
- **L3, DAW** : Reaper sur Windows 11 x64 et macOS (Apple Silicon, Intel si supporté), checklist, captures, projets et rendus. Manuel.
- **L4, serveur** : staging, charge synthétique, restauration, rotation de clé, worker hostile.

L1 ne prouve rien sur le DAW ; L3 manuel ne protège pas d'une régression. Les preuves des quatre niveaux sont liées au même artefact.

## 2. Matrice exigences, tests, preuves, blocage

| Exigence | Test | Preuve attendue | Bloquant |
|---|---|---|---|
| Effet stéréo et synthé poly, audio et MIDI | L2 + L3 | rendus WAV/MIDI, 16 voix, zéro dropout | M1 |
| Paramètres et automation stables après mise à jour | L1 manifeste, L2 set/save/reopen | projet avant/après, index et valeurs identiques | M1 |
| Recall, sauvegarde, réouverture, autre machine | L3 | projet + rendu de référence, écart documenté | M1 |
| Audio et MIDI fenêtre fermée | L2 + L3 | audio continu, delta CPU mesuré, rendu identique | M1 |
| Rendu hors ligne | L2 + L3 | aucun bloc perdu ni silence de timeout, durée et écart numérique documentés ; accélération mesurée, pas exigée universellement | M1 |
| Hors ligne total, serveur et DNS coupés | L2 + L3 | installation locale, bibliothèque utilisable | M1 |
| Versions côte à côte, pas de mise à jour silencieuse | L1 install, L2 deux versions, L3 projet | même ID module, versions et hashes distincts, ancien projet intact | M1 |
| Installation concurrente multi-instances | L2 stress deux processus | aucun partiel, un écrivain, attente ou erreur propre | M1 |
| Paquet hostile : traversal, symlink, bomb, collision, Unicode, ZIP64 | L1 corpus + fuzz | tout rejeté, aucune écriture hors staging | M1 |
| Digest, signature, clé révoquée, rotation | L1 clés de test | altération détectée, nouvelle installation refusée | M2 |
| Licences, ressources, auteur déclaré | L1 + revue humaine | champs complets, ressource sans licence rejetée | M2 |
| Accessibilité et clavier | L3 clavier seul + NVDA/VoiceOver | parcours complet, focus ordonné, pas de couleur seule | M1 |
| Builds signés et notarized | CI + manuel | Authenticode, codesign/notarization, Gatekeeper | M3 |
| Quotas et anti-abus | L4 | 429 et quotas respectés, pas de famine | M2 |
| Retrait public et préservation projet | L3 | version exacte rouverte, état intact | M2 |
| Signalement et journal | L1 + L4 | signalement visible, décision et motif journalisés | M2 |
| Performances CPU et latence | L2 + L3 | durée callback médiane/p95/p99/max, échéances, xruns, PDC, charge du cœur audio ; protocole ci-dessous | M1 |
| Confinement du code tiers | L2 + L3 + L4 adversarial | crash/boucle/IPC malformé contenus, aucune lecture de secrets ni sortie réseau ; restrictions OS effectivement vérifiées | avant tout test de code tiers, y compris alpha |

## 3. Budgets de performance proposés

Aucun chiffre acquis. Machines de référence : A, Windows 11, 8 cœurs, 16 Go, SSD ; B, macOS 14, Apple M1, 8 Go. Protocole : 48 kHz, buffers 64/128/256, huit instances de l'effet de référence, un synthé 16 voix, dix minutes, cinq répétitions ; mesurer CPU total, xruns, charge DSP maximale, delta UI fermée/ouverte. Rapporter médiane et p95, jamais un essai unique.

Ne pas utiliser un pourcentage CPU global comme seul seuil : il peut masquer la saturation du cœur audio. Pour chaque build et machine réellement disponible, consigner CPU exact, OS, interface/pilote, fréquence, buffer et plan énergie. Mesurer callback médiane/p95/p99/max, échéances manquées, xruns, allocations et verrous, latence IPC et PDC à l’impulsion. Aucun xrun imputable au plugin admis dans la campagne de référence ; la marge callback cible doit être fixée et justifiée après baseline, avant gel du moteur. Mesurer 32/64/128/256 et blocs variables, puis 16 si supporté ; fréquences 44,1/48/96 kHz. Les machines A/B ci-dessus sont des profils proposés, pas du matériel disponible vérifié. Installation et recherche : mesurer taille réelle, disque et réseau ; seuils UX après baseline, aucun chiffre acquis.

## 4. Ce qu'un test n'établit pas

- Un corpus hostile réussi ne prouve pas l'absence de faille du parseur, du JIT ou du runtime.
- Un faux serveur L1 ne prouve ni le vrai serveur, ni TLS, ni la charge.
- Un rendu L2 ne couvre ni tous les DAW, ni buffers, taux et cartes audio.
- Une checklist L3 sur une machine ne couvre pas la diversité matérielle.
- Une signature prouve origine et intégrité, pas innocuité.
- Un champ licence rempli ne prouve pas les droits sur les assets.
- Une charge synthétique ne prédit pas l'échelle communautaire.
- Aucun test ne garantit qu'un module malveillant ne s'échappe pas du runtime.

## 5. Gates

- **G0, prototype privé** : aucun serveur public ni compte. Périmètre M1, L1/L2 verts, checklist L3 sur les deux OS, corpus hostile rejeté. Sortie : démonstration reproductible, limites écrites. Ne prouve pas la sécurité.
- **G1, alpha** : testeurs invités, serveur privé, comptes et soumissions, Unapproved masqué. Exige les tests bloquants M1/M2, restauration réussie, rotation de clé exercée, journal de modération, politique de signalement. Sortie : parcours complet avec un vrai paquet et un vrai rejet. Ne prouve pas l'échelle.
- **G2, communauté publique** : inscription ouverte, quotas, anti-abus, runbook d'incident, builds signés et notarized, audit de licences, confidentialité, modération staffée. Exige tous les tests bloquants, un exercice d'incident, une revue externe si possible. Sortie : validation explicite du mainteneur pour l’ouverture publique, pas un verdict autonome des agents.

## 6. Risques prioritaires

1. Évasion du runtime ou code hostile : non résolu, à traiter avant toute exécution tierce, même chez des testeurs invités. Un prototype privé peut uniquement exécuter des fixtures de confiance.
2. Licence Cmajor ou API hôte incompatible avec la distribution open source.
3. Fuite de jeton ou de privilège réseau par l'UI communautaire.
4. Confusion hash, version et approbation lors d'un remplacement.
5. Verrous Windows, antivirus, mises à jour partielles.
6. Projet cassé après retrait ou suppression.
7. Modération sous-dimensionnée face au volume.
8. Assets sans droits clairs malgré un champ licence rempli.
9. Serveur unique : panne, sauvegarde, saturation.
10. Performances inconnues à petits buffers, surtout en polyphonie.

## 7. Décisions absentes

- Moteur DSP et politique de version de l'API hôte.
- Propriétaire et limites du bac à sable runtime.
- Matrice OS, architectures, DAW et versions minimales.
- Garde des clés, reproductibilité, coût des certificats.
- Licence du projet, CLA, compatibilité des dépendances.
- Rétention, confidentialité, suppression de compte.
- Politique de modération, délais, appel, transparence.
- Seuils de performance et matériel de référence.
- Budget d'hébergement, de signature et de revue externe.

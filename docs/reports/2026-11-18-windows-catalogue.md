# Validation Windows du jalon catalogue, 2026-11-18

Commit de depart : `c32fed9` sur `main`, plus les corrections d'interface
decrites plus bas. Machine cible : DESKTOP-SC9LHNB, Windows 10.0.26300.9974,
REAPER 7.85 (`C:\Program Files\REAPER (x64)\reaper.exe`), Visual Studio 2026,
CMake 4.4.3, Python 3.14.7.

## Ce qui a ete construit et installe

Deux etats se succedent sur cette machine, et il faut les distinguer.

**Build valide par rendu** (installe pendant les mesures) : SHA-256
`8a55b4738575a3a83bb2773b0c938d314c404b1516a9c6733722afca8814c7ee` (FX) et
`6e26bebff2ebaa0dc30c0b32b7d1ff2e61608611441eedefd74bfb634a56023c`
(Instrument). Toutes les mesures de rendu et de module de ce rapport portent sur
ce build.

**Build apres corrections d'interface** : SHA-256
`a8cb72460834c73eadb153d358fddfebb234d511d087f7d823741e2faec70526` (FX) et
`4b858c8de7b84e372b30e589c00bbfbe5666557fb6108a38dc6df00d368bd319`
(Instrument). Compile sur la machine cible, **jamais installe** : un DLL charge
ne peut pas etre remplace, et le REAPER d'Arthur est reste ouvert. La tache
`OpenVST-InstallWhenIdle` armee pour l'installer a la premiere fermeture a ete
desarmee apres la verification visuelle du 19/11 (voir plus bas), pour que le
binaire installe reste exactement celui que couvrent les rendus. Les fichiers
touches par ce second build sont uniquement `NativeEditor.h`, `ModuleUI.*`,
`CataloguePanel.h` ; aucun rendu Windows ne le couvre, et il n'est pas installe.

Installation dans le dossier par utilisateur
`%LOCALAPPDATA%\Programs\Common\VST3`, deja present dans le `vstpath64`
d'Arthur. Chaine de verification de l'installeur : copie vers un nom de staging,
comparaison SHA-256 source/staging, remplacement, relecture du binaire installe,
puis controle de chaines propres a ce commit (`openvst.ui/0`,
`openvst.fx.module.8.v1`, `Press Refresh to list approved modules.`,
`openvst.instr.module.8.v1`) et rejet de l'ancienne chaine `Ready ?`.

## Rendu Windows compare au rendu Linux

Le profil de test pointe sur le dossier VST3 installe uniquement, et le script
affiche le chemin scanne. Les projets `.rpp` sont produits par REAPER Linux puis
rendus sur Windows, le format et l'etat plugin etant portables.

| Verification | Windows | Linux |
|---|---|---|
| FX gain 0.5, entree L=0.2 / R=-0.3 | L=0.100006103515625, R=-0.149993896484375 | identique |
| Erreur maximale FX | 6.103515624994449e-06 | 6.103515624994449e-06 |
| Instrument, accord 60/64/67 | onset 26, peak 0.188582181930542 | identique |
| Automation 0 vers 1 | rampe monotone, 0.0427 vers 0.400 | identique |

## Module Tremolo installe par le catalogue

Le paquet de demonstration est construit de facon reproductible : deux
constructions des memes sources donnent le meme SHA-256
`c6055758f5e69ab7a5a8052454655a6d41b2b01d21fdbf0b04f671df8d626551`
(1 796 octets). Le generateur ecrivait auparavant l'horodatage des fichiers dans
le ZIP : deux constructions de la version 1.0.0 produisaient deux paquets
differents, ce qui contredisait l'invariant de version immuable.

Sonde headless sur Windows, sans serveur, avec le meme paquet :

    openvst_catalogue_probe.exe --store %APPDATA%\OpenVST\modules \
      --runtime cmajor-win\CmajPerformer.dll --fixtures fixtures \
      --id org.openvst.demo.tremolo --package tremolo-1.0.0.openvst.zip \
      --state-out tremolo-state-win.json
    openvst_catalogue_probe.exe ... --offline 1 --state-in tremolo-state-win.json

Sortie : `OK id=org.openvst.demo.tremolo digest=c6055758... peak=0.841865`,
code 0 dans les deux cas. Le second appel n'a aucun serveur : l'installation et
l'etat viennent du store seul.

Projet REAPER : l'etat enregistre par le plugin (schema 3, module Tremolo,
`speed=5.0`, `amount=0.75`, `level=1.5`) est injecte via le parametre nomme
`vst_chunk` de REAPER, c'est-a-dire le meme chemin que l'ouverture d'un projet
sauvegarde. Le projet est ensuite enregistre par REAPER lui-meme.

| Mesure | Valeur |
|---|---|
| Module relu dans le chunk du projet | `org.openvst.demo.tremolo` |
| Valeurs d'etat conservees | intacts apres sauvegarde et reouverture |
| Slots generiques cote hote (normalises) | 0.246231169 / 0.750000000 / 0.750000000 |
| Erreur max contre le modele analytique (float32) | 5.960464477539063e-08 |
| Difference Windows / Linux apres quantification 16 bits | 0.0 |

Le modele analytique reproduit l'arithmetique du module en float32, phase
accumulee comprise. Un modele en double precision derivait de 5.2e-04 sur une
seconde, au-dessus de la quantification, et aurait masque un vrai defaut.

## Interface : defauts trouves et corriges

L'editeur a ete rendu hors ecran par un nouvel outil
(`build/plugin/openvst_editor_snapshot`), qui construit le vrai editeur et le
peint dans une image PNG sous Xvfb. C'est mesurable, contrairement a une capture
de fenetre de DAW.

Defauts constates sur le rendu de `c32fed9` :

1. Aucun fond opaque : sous Linux, la fenetre du plugin laissait voir le bureau
   et les dialogues REAPER a travers le panneau. Les groupes et les libelles
   perdaient tout contraste.
2. Le cadre de groupe etait dessine sur toute la hauteur, son titre tombait sur
   la bordure arrondie, et les lignes de valeur blanches etaient invisibles sur
   le fond blanc du groupe.
3. Les boutons rotatifs affichaient le minimum de leur plage jusqu'au premier
   tick du minuteur 30 Hz, au lieu de la valeur reelle.
4. Le panneau Discover n'avait pas de fond propre et ses libelles de statut et
   de details etaient invisibles.

Corrections : fond opaque et palette explicite dans `ModuleUI` et
`CataloguePanel` (le format ne donne aucune couleur a l'auteur, donc le conteneur
doit en garantir la lisibilite), cadre de groupe decale sous la ligne de titre,
valeurs initiales posees a la construction, couleurs de texte explicites.

Apres correction, la capture hors ecran montre des libelles lisibles
(Rate / Depth / Output), leurs valeurs (2.000 Hz / 0.500 / 1.000), le groupe
Modulation avec ses trois lignes, et un panneau Discover qui affiche
`Installed and selected: Tremolo` puis `Ready - org.openvst.demo.tremolo`.
Le panneau Discover a ete exerce par son propre code : `refreshNow()`,
selection de la premiere entree, `installNow()`, sur le serveur de test local.

### Correction : ce que la capture hors ecran exagerait

Le rendu hors ecran peint le panneau sur une image vide, sans fond de fenetre.
Sur cette image, les blancs translucides devenaient du blanc plein et le texte
blanc disparaissait : les defauts 2 et 4 paraissaient donc plus graves qu'en
usage. Dans un vrai hote qui peint du noir derriere le plugin, ces deux points
se lisaient deja correctement. Le seul cas reellement casse etait la fenetre
Linux transparente, ou le bureau et les dialogues REAPER traversaient le
panneau. La correction reste justifiee, mais la capture hors ecran ne doit pas
etre presentee comme la preuve d'un defaut visible chez l'utilisateur.

## Verification visuelle dans le REAPER d'Arthur, 19/11

Capture : `docs/reports/2026-11-19-windows-tremolo.png`.

- Hote : `C:\Program Files\REAPER (x64)\reaper.exe`, REAPER 7.85, son
  installation, sa configuration. Aucun profil de test, aucun `-cfgfile`.
- Binaire charge : `8a55b473…` (FX), celui que couvrent tous les rendus.
- Observé : `Ready - org.openvst.demo.tremolo`, SHA-256 du module, selecteur
  `Tremolo`, bouton `Discover`, trois boutons rotatifs avec libelles et valeurs
  (2.000 Hz / 0.500 / 1.000), groupe `Modulation` avec ses trois lignes
  lisibles, ligne de description, aucun texte coupe, aucun chevauchement.
- Ce que cela prouve : le panneau declaratif se rend nativement sous Windows,
  dans le vrai hote, avec le module installe par le catalogue selectionne et
  prepare. C'est la verification visuelle qui manquait au jalon.
- Ce que cela ne prouve pas : le clic sur `Discover`, `Refresh` et `Install`
  n'a pas ete filme dans son REAPER ; ce parcours reste prouve par le code du
  panneau hors ecran et par la sonde headless. Le rendu a d'autres facteurs
  d'echelle DPI n'a pas ete photographie.

Aucune information ne depend de la couleur : les etats sont portes par du texte
et des positions, jamais par une teinte.

## Incidents de methode, a ne pas repeter

1. **`kill reaper` a tue la session d'Arthur.** Une commande de l'agent
   interactif tue les processus par nom. Elle a ferme son REAPER ouvert sur un
   projet modifie. Aucune perte declaree par Arthur, mais la commande ne doit
   plus exister sous cette forme : filtrer sur la ligne de commande du profil de
   test.
2. **Dialogue « no audio device selected ».** Un profil de test sans
   peripherique audio affiche une boite modale qui bloque le rendu. Le worker
   repond maintenant Non a cette boite sur sa propre instance, par identifiant
   de processus, et lance REAPER avec `-noactivate`.
3. **`-newinst` relance un processus.** Le PID retourne par `Start-Process`
   n'est pas celui qui possede la fenetre. L'identification se fait par ligne de
   commande, pas par PID de lancement.
4. **Les fenetres REAPER genent Arthur.** Les tests de rendu ouvrent des
   fenetres sur son bureau. Ils doivent etre groupes, lances avec
   `-noactivate`, et jamais repetes sans raison. La verification d'interface
   passe par le rendu hors ecran.

## Non teste, a ne pas presenter comme acquis

- Le parcours `Discover` / `Refresh` / `Install` n'a pas ete clique dans le
  REAPER d'Arthur ; il est prouve par le code du panneau hors ecran et par la
  sonde headless, pas par une capture de son ecran.
- Le rendu de l'editeur a un facteur d'echelle DPI autre que celui de sa
  machine n'a pas ete photographie.
- Pas de mesure temps reel : ni temps de callback, ni xruns, ni PDC.
- Pas de macOS.
- Pas de bac a sable, pas de signature : Cmajor compile la source du paquet
  dans le processus.
- Le panneau catalogue fait toujours son reseau et son installation sur le fil
  de message.

## Proposition de mise a jour d'AGENTS.md (non appliquee)

La section « Known next step » est perimee : la selection de module et le
parcours catalogue sont faits et verifies sur les deux plateformes. Proposition
de remplacement :

    ## Known next step

    The catalogue path is verified end to end on Linux and Windows: install,
    module selection by stable identity and per-module hash, render, offline
    reopen with the same values. Module selection is done; do not re-plan it.
    What is still missing is a measurement of real-time behaviour (callback
    time, xruns, PDC), a macOS build, and a visual check of the editor on the
    target platform.

# Def Jam: Fight for NY — recompilation statique

Une recompilation statique de **Def Jam: Fight for NY** (Xbox, version USA) en programme Windows natif. Le code du jeu est traduit en C, puis compilé sur votre PC à partir de votre propre image disque.

> **Aucun contenu du jeu n'est fourni.** Ce dépôt et ses releases ne contiennent ni code ni données du jeu. L'installeur lit votre image disque, la vérifie, l'extrait et reconstruit tout sur votre machine. N'utilisez que l'image disque d'un jeu que vous possédez.

*English summary at the end.*

## Ce qu'il vous faut

- Windows 10 ou 11, 64 bits, et une carte graphique DirectX 11.
- L'image disque Xbox (`.iso`) de Def Jam: Fight for NY, **version USA**. L'installeur la vérifie : la version Europe et toute autre sont refusées.
- Environ 4 Go libres.
- Une manette de type Xbox (XInput). Le clavier ne pilote pas le jeu.

## Installer

### 1. Avec DJFFNY.exe : le plus simple

Téléchargez **`DJFFNY.exe`** dans la dernière [release](../../releases/latest) et lancez-le. Il fait tout, dans une seule fenêtre :

- **l'installation** : dossier, version (Clang tout compris, ou MSVC + DLSS), image disque vérifiée, puis téléchargement de la release choisie, extraction, traduction en C et compilation, avec la progression et le journal ;
- **les réglages** du jeu (résolution, 4:3 ou 16:9, DLSS, filtrage, pack de textures HD, tout débloquer, manette) ;
- **le mod** : installez un fichier `.mod` (un zip avec un `mod.ini`), un seul actif à la fois, retirable ;
- **le jeu** : le bouton Jouer, et l'annonce des mises à jour.

Il s'installe à côté du jeu avec un raccourci sur le Bureau. Il utilise WebView2, déjà présent sur Windows 10 et 11.

### 2. Avec une release, à la main

Il y a deux paquets dans chaque release. Choisissez-en un dans [Releases](../../releases), dézippez-le, puis double-cliquez **`Installer.bat`** et choisissez votre image disque. L'extraction, la traduction en C et la compilation prennent quelques minutes.

| Release | Compilateur | DLSS | À installer sur le PC |
|---|---|---|---|
| **MSVC + DLSS** (léger, ~2 Mo) | Visual Studio | Oui, avec une carte NVIDIA RTX | Visual Studio Build Tools (C++), Python 3 avec `capstone`, CMake. S'il en manque, l'installeur propose de les installer avec winget, seulement si vous acceptez. |
| **Clang intégré** (complet, ~100 Mo) | Clang (llvm-mingw), fourni | Non | Rien : Clang, Python, capstone et CMake sont dans le dossier `outils`. |

**Le DLSS** : avec la release MSVC et une carte NVIDIA RTX, l'installeur propose de télécharger NVIDIA Streamline 2.14.1 (276 Mo) depuis [son dépôt officiel](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1). Il vérifie l'empreinte de l'archive avant de l'utiliser. La licence de NVIDIA interdit de distribuer ce SDK à part, c'est pourquoi il n'est pas inclus dans la release.

### 3. Depuis les sources : recompiler l'installeur vous-même

Le portage a besoin du toolkit [xboxrecomp](https://github.com/comoria08/xboxrecomp/tree/defjam-bringup), un fork de [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) qui contient les correctifs du portage. Clonez les deux dépôts **côte à côte** :

```
git clone https://github.com/comoria08/defjam_recomp.git
git clone -b defjam-bringup https://github.com/comoria08/xboxrecomp.git
```

Puis double-cliquez `defjam_recomp\installer.bat`, ou lancez dans PowerShell :

```
.\defjam_recomp\installeur\installer.ps1 -Iso "C:\chemin\vers\DefJam.iso"
```

| Option | Effet |
|---|---|
| `-Iso <chemin>` | l'image disque (sinon une fenêtre la demande) |
| `-Oui` | installe les outils manquants et le DLSS sans poser la question |
| `-SansDLSS` | compile sans DLSS, sans rien proposer |
| `-Outils <dossier>` | utilise des outils embarqués (Clang, Python, CMake) au lieu de ceux de la machine |
| `-Toolkit <dossier>` | un toolkit ailleurs qu'à côté du projet |

Tout est écrit dans `defjam_recomp\installation.log`.

**Fabriquer les releases vous-même** :

```
.\paquet\fabriquer_paquet.ps1                 # release MSVC + DLSS
.\paquet\preparer_outils.ps1                  # télécharge et vérifie Clang, Python, capstone, CMake
.\paquet\fabriquer_paquet.ps1 -Complet        # release Clang intégré
```

Les paquets sont écrits dans `paquet\sortie\`. Ils sont toujours tirés des commits (`git archive`), jamais des fichiers en cours de modification.

**Compiler DJFFNY.exe** (Visual Studio, et le SDK WebView2 de Microsoft, paquet NuGet `Microsoft.Web.WebView2`, extrait dans `tiers\webview2\sdk`) :

```
cmake -S lanceur -B build_lanceur -A x64
cmake --build build_lanceur --config Release
```

**Fabriquer un mod** : `outils\faire_mod.py <dossier mods> <sortie.mod> --nom "..."` emballe un dossier `game_files\mods` en `.mod`.

## Jouer

Lancez DJFFNY.exe (raccourci « Def Jam Fight for NY » sur le Bureau), ou `build\Release\defjam_recomp.exe`. Une fenêtre de réglages s'ouvre d'abord : résolution, 4:3 ou 16:9, plein écran, filtrage des textures, effets de rendu et mods. F11 bascule le plein écran.

- Sauvegardes : `game_files\UDATA`
- Mods : `game_files\mods` (un fichier placé là remplace celui du disque)

## Licences

- Ce portage : MIT ([LICENSE](LICENSE)).
- Le toolkit xboxrecomp : MIT, avec des éléments sous LGPL-2.1 et GPL-2.0 repris de xemu et de Hatari (voir `NOTICE` et `LICENSES` dans le toolkit).
- Le programme compilé sur votre PC n'en lie que la partie LGPL (le NV2A) : le son passe par la couche DirectSound du toolkit (`src/dsound_hle`, MIT), et l'émulation de l'APU, cœur DSP GPL compris, n'est plus construite. Il contient le code traduit du jeu : il est pour votre usage, ne le partagez pas.
- NVIDIA Streamline et le DLSS, s'ils sont téléchargés : sous les licences de NVIDIA, jointes à leurs fichiers dans `tiers\streamline`.

Ce projet n'est ni affilié à Electronic Arts, AKI Corporation, Def Jam Recordings, Microsoft ou NVIDIA, ni approuvé ou soutenu par eux. Les marques citées appartiennent à leurs propriétaires.

**Remerciements** : xboxrecomp, de sp00nz et ses contributeurs, dont fearkov pour la compilation MinGW et le reset AC'97 ; le code audio MCPX de xemu (espes, Jannik Vogel, Matt Borgerson), d'après Hatari et ARAnyM ; capstone ; llvm-mingw, de Martin Storsjö ; Python ; CMake.

---

## English

A static recompilation of Def Jam: Fight for NY (Xbox, USA) into a native Windows program, built on your PC from your own disc image. **No game content is included.**

- **Easiest**: download `DJFFNY.exe` from the [latest release](../../releases/latest) and run it. It installs the game (Clang or MSVC + DLSS build, disc image check, download, translation, build), edits the settings, installs a `.mod` (a zip with a `mod.ini`, one at a time) and launches the game. It needs WebView2, already part of Windows 10 and 11.
- **From a release package**: download one of the two packages, unzip it, double-click `Installer.bat` and pick your `.iso`.
  - *MSVC + DLSS* needs Visual Studio Build Tools, Python 3 with capstone, and CMake. The installer offers to install them with winget. On an NVIDIA RTX card it also offers to download NVIDIA Streamline (DLSS) from NVIDIA's official repository.
  - *Built-in Clang* brings all its tools, so there is nothing to install. It has no DLSS.
- **From source**: clone this repository and [the xboxrecomp fork](https://github.com/comoria08/xboxrecomp/tree/defjam-bringup) (branch `defjam-bringup`) side by side, then run `installer.bat` or `installeur\installer.ps1 -Iso <path>`. Use `-NoDLSS` to skip DLSS and `-Yes` to accept all downloads.

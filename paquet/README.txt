{NOM}
==========================================================================

Français plus bas.

A static recompilation of Def Jam: Fight for NY (Xbox, USA) into a native
Windows program. The game's code is translated to C and compiled on your own
PC, from your own disc image.


NO GAME CONTENT IS INCLUDED
--------------------------
This package contains no code and no data from the game. The installer reads
your disc image, checks it, extracts it and rebuilds everything on your
machine. Use only a disc image of a game you own.

This project is not affiliated with, endorsed or sponsored by Electronic Arts,
AKI Corporation, Def Jam Recordings or Microsoft. Def Jam, Fight for NY, EA,
Xbox and all other trademarks belong to their respective owners.


WHAT YOU NEED
-------------
- Windows 10 or 11, 64-bit, and a graphics card with DirectX 11.
- The Xbox disc image (.iso) of Def Jam: Fight for NY, USA version.
  The installer checks it: the Europe version and any other are refused.
- About 4 GB free next to this folder.
- An Xbox-style controller (XInput). There is no keyboard control.
- Building tools. The full package brings its own in its "outils" folder
  (Clang from llvm-mingw, Python, capstone, CMake): nothing to install.
  The light package needs Visual Studio Build Tools (C++), Python 3 with the
  capstone module, and CMake; if some are missing, the installer offers to
  install them with winget, only if you agree.


INSTALL
-------
1. Double-click Installer.bat.
2. Choose your disc image.
3. Wait a few minutes: extraction, translation to C, build.
Everything is written to defjam_recomp\installation.log.


PLAY
----
Double-click the "Def Jam Fight for NY" shortcut in this folder.
A settings window opens first (resolution, 4:3 or 16:9, full screen, texture
filtering, rendering effects, mods). Tick "Don't show this launcher again"
to skip it; hold Shift while starting the game to see it again.
In 16:9, "Interface and menus in 4:3" keeps the menus and the HUD unstretched
while the fights fill the screen.
F11 switches full screen.
On the fighter select, Y picks a fighter at random.

Saves:  defjam_recomp\game_files\UDATA
Mods:   defjam_recomp\game_files\mods  (a file there replaces the disc's)


LICENCES
--------
- This port: MIT (LICENSE).
- The xboxrecomp toolkit: MIT, with components under the LGPL-2.1 and the
  GPL-2.0 taken from xemu and Hatari (xboxrecomp\NOTICE, xboxrecomp\LICENSES).
- The program built on your PC links that GPL code, and it contains the
  game's translated code: it is for your own use, do not share it.
- The "outils" folder of the full package, trimmed of what the build does
  not use, with Python's search path set: llvm-mingw (Apache-2.0 with LLVM
  exception, and the mingw-w64 licences), Python (PSF), capstone (BSD),
  CMake (BSD-3). Each licence sits next to its tool; outils\OUTILS.txt says
  where.

Credits: xboxrecomp by sp00nz and its contributors, including fearkov for the
MinGW build and the AC'97 reset; the MCPX audio code of xemu (espes, Jannik
Vogel, Matt Borgerson), after Hatari and ARAnyM; capstone; llvm-mingw by
Martin Storsjö; Python; CMake.


==========================================================================
FRANÇAIS
==========================================================================

Une recompilation statique de Def Jam: Fight for NY (Xbox, USA) en programme
Windows natif. Le code du jeu est traduit en C puis compilé sur votre PC, à
partir de votre propre image disque.


AUCUN CONTENU DU JEU N'EST FOURNI
---------------------------------
Ce paquet ne contient ni code ni données du jeu. L'installeur lit votre image
disque, la vérifie, l'extrait et reconstruit tout sur votre machine.
N'utilisez que l'image disque d'un jeu que vous possédez.

Ce projet n'est ni affilié à Electronic Arts, AKI Corporation, Def Jam
Recordings ou Microsoft, ni approuvé ou soutenu par eux. Def Jam, Fight for
NY, EA, Xbox et les autres marques appartiennent à leurs propriétaires.


IL VOUS FAUT
------------
- Windows 10 ou 11, 64 bits, et une carte graphique DirectX 11.
- L'image disque Xbox (.iso) de Def Jam: Fight for NY, version USA.
  L'installeur la vérifie : la version Europe et toute autre sont refusées.
- Environ 4 Go libres à côté de ce dossier.
- Une manette de type Xbox (XInput). Le clavier ne pilote pas le jeu.
- Des outils de compilation. Le paquet complet apporte les siens dans son
  dossier « outils » (Clang de llvm-mingw, Python, capstone, CMake) : rien
  à installer. Le paquet léger demande Visual Studio Build Tools (C++),
  Python 3 avec le module capstone, et CMake ; s'il en manque, l'installeur
  propose de les installer avec winget, seulement si vous acceptez.


INSTALLER
---------
1. Double-cliquer Installer.bat.
2. Choisir votre image disque.
3. Attendre quelques minutes : extraction, traduction en C, compilation.
Tout est écrit dans defjam_recomp\installation.log.


JOUER
-----
Double-cliquer le raccourci « Def Jam Fight for NY » de ce dossier.
Une fenêtre de réglages s'ouvre d'abord (résolution, 4:3 ou 16:9, plein
écran, filtrage des textures, effets de rendu, mods). Cocher « Ne plus
afficher ce lanceur » pour la sauter ; garder Maj enfoncée au lancement pour
la revoir.
En 16:9, « Interface et menus en 4:3 » garde les menus et l'interface non
étirés, les combats remplissant l'écran.
F11 bascule le plein écran.
Sur l'écran de sélection des combattants, Y en choisit un au hasard.

Sauvegardes : defjam_recomp\game_files\UDATA
Mods :        defjam_recomp\game_files\mods  (un fichier y remplace celui du disque)


LICENCES
--------
- Ce portage : MIT (LICENSE).
- Le toolkit xboxrecomp : MIT, avec des éléments sous LGPL-2.1 et GPL-2.0
  repris de xemu et de Hatari (xboxrecomp\NOTICE, xboxrecomp\LICENSES).
- Le programme compilé sur votre PC contient ce code GPL et le code traduit
  du jeu : il est pour votre usage, ne le partagez pas.
- Le dossier « outils » du paquet complet, allégé de ce que la compilation
  n'utilise pas, avec le chemin de recherche de Python réglé : llvm-mingw
  (Apache-2.0 avec exception LLVM, et les licences de mingw-w64), Python
  (PSF), capstone (BSD), CMake (BSD-3). Chaque licence est à côté de son
  outil ; outils\OUTILS.txt dit où.

Remerciements : xboxrecomp, de sp00nz et ses contributeurs, dont fearkov pour
la compilation MinGW et le reset AC'97 ; le code audio MCPX de xemu (espes,
Jannik Vogel, Matt Borgerson), d'après Hatari et ARAnyM ; capstone ;
llvm-mingw, de Martin Storsjö ; Python ; CMake.

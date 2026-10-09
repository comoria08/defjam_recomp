@echo off
rem  {NOM} : installs the game from your disc image / installe le jeu depuis
rem  votre image disque. See README.txt / voir README.txt.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0defjam_recomp\installeur\installer.ps1" -Raccourci "%~dp0." %*
echo.
pause

@echo off
rem  Installe Def Jam: Fight for NY (recompilation statique) a partir de l'image
rem  disque du joueur : extraction, recompilation, compilation (point 142).
rem  Double-cliquer ce fichier : une fenetre demande l'image disque (.iso).
rem  Tout est ecrit dans installation.log.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installeur\installer.ps1" %*
echo.
pause

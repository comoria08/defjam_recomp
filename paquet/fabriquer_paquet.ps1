# fabriquer_paquet.ps1 - le paquet public, dans paquet\sortie\ (point 143).
#
# Ne publie rien : il assemble un dossier et son zip.
#
#   <Nom>\Installer.bat        le seul fichier a double-cliquer
#   <Nom>\README.txt           anglais puis francais
#   <Nom>\LICENSE              MIT, celle du portage
#   <Nom>\VERSION.txt          les commits d'ou vient le paquet
#   <Nom>\defjam_recomp\       le portage, sur une liste fermee (ci-dessous)
#   <Nom>\xboxrecomp\          le toolkit entier, avec LICENSE, NOTICE, LICENSES\
#
# Tout vient de commits (git archive), jamais de l'arbre de travail : le paquet
# est celui qu'un autre obtiendrait des memes commits. Rien du jeu : ni C
# genere, ni game_files, ni pack HD, ni mods ; ni les outils de mod, ni le
# guide, ni le .env de cette machine.
#
# Usage : .\paquet\fabriquer_paquet.ps1 [-Nom "DJFFNY Recomp"] [-Toolkit D:\xboxrecomp] [-ToolkitRef HEAD]
# Pour essayer un paquet avant de commiter : -ToolkitRef (git stash create) et
# -PortRef (git stash create), apres un git add des fichiers nouveaux.
# -Complet ajoute outils\ (paquet\outils, fait par preparer_outils.ps1) : Clang,
# Python, capstone et CMake embarques, rien a installer chez le joueur (point 145).
param(
    [string]$Nom = "DJFFNY Recomp",
    [string]$Toolkit = "",
    [string]$ToolkitRef = "HEAD",
    [string]$PortRef = "HEAD",
    [switch]$Complet
)
$ErrorActionPreference = "Stop"

$Projet = Split-Path -Parent $PSScriptRoot
if (-not $Toolkit) { $Toolkit = Join-Path (Split-Path -Parent $Projet) "xboxrecomp" }
$Sortie  = Join-Path $PSScriptRoot "sortie"
$Dossier = ($Nom -replace '\s+', '-') + $(if ($Complet) { "-complet" } else { "" })
$OutilsSrc = Join-Path $PSScriptRoot "outils"
if ($Complet -and -not (Test-Path (Join-Path $OutilsSrc "llvm-mingw\bin\clang.exe"))) {
    throw "pas d'outils dans $OutilsSrc : lancer d'abord paquet\preparer_outils.ps1"
}
$Dest    = Join-Path $Sortie $Dossier
$Zip     = Join-Path $Sortie "$Dossier.zip"

# Les fichiers du portage qu'un joueur recoit : de quoi compiler et installer.
$Portage = @(
    "CMakeLists.txt", "LICENSE",
    "src/main.c", "src/recomp_manual.c", "src/lanceur.c", "src/lanceur.h",
    "src/pad_aleatoire.c", "src/pad_aleatoire.h",
    "icall_seeds.json", "xpp_seeds.json", "interp_seeds.json", "misc_seeds.json",
    "installeur/installer.ps1"
)

function Commit($depot, $ref) { (git -C $depot rev-parse --short $ref).Trim() }
function Sale($depot) { (git -C $depot status --porcelain --untracked-files=no) -ne $null }

$cTool = Commit $Toolkit $ToolkitRef
$cPort = Commit $Projet $PortRef
foreach ($d in @(@($Toolkit, $ToolkitRef), @($Projet, $PortRef))) {
    if ($d[1] -eq "HEAD" -and (Sale $d[0])) {
        Write-Host "attention : $($d[0]) a des modifications non commitees, elles ne seront pas dans le paquet" -ForegroundColor Yellow
    }
}

if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
if (Test-Path $Zip)  { Remove-Item -Force $Zip }
New-Item -ItemType Directory -Force (Join-Path $Dest "xboxrecomp"), (Join-Path $Dest "defjam_recomp") | Out-Null

# git archive ecrit un tar sur stdout ; PowerShell 5.1 abime les octets d'un
# tube binaire, donc un fichier intermediaire.
function Extraire($depot, $ref, $vers, [string[]]$chemins) {
    $tar = Join-Path $Sortie "tmp.tar"
    git -C $depot archive --format=tar -o $tar $ref -- @chemins
    if ($LASTEXITCODE -ne 0) { throw "git archive a echoue dans $depot" }
    # Le tar de Windows : celui de Git lirait "C:" comme un hote distant.
    & "$env:SystemRoot\System32\tar.exe" -x -f $tar -C $vers
    if ($LASTEXITCODE -ne 0) { throw "tar a echoue" }
    Remove-Item $tar
}
Extraire $Toolkit $ToolkitRef (Join-Path $Dest "xboxrecomp") @(".")
Extraire $Projet $PortRef (Join-Path $Dest "defjam_recomp") $Portage
$racine = Join-Path $Sortie "tmp_racine"
if (Test-Path $racine) { Remove-Item -Recurse -Force $racine }
New-Item -ItemType Directory -Force $racine | Out-Null
Extraire $Projet $PortRef $racine @("paquet/README.txt", "paquet/Installer.bat")

# Les fichiers de la racine, avec le nom du paquet.
foreach ($f in @("README.txt", "Installer.bat")) {
    $t = [IO.File]::ReadAllText((Join-Path $racine "paquet\$f"), [Text.Encoding]::UTF8).Replace("{NOM}", $Nom)
    if ($f -like "*.bat") {
        # cmd lit un .bat dans la page de code de la console : de l'ASCII et des CRLF.
        $t = $t -replace "`r?`n", "`r`n"
        [IO.File]::WriteAllText((Join-Path $Dest $f), $t, [Text.Encoding]::ASCII)
    } else {
        $t = $t -replace "`r?`n", "`r`n"
        [IO.File]::WriteAllText((Join-Path $Dest $f), $t, (New-Object Text.UTF8Encoding $true))
    }
}
Remove-Item -Recurse -Force $racine
Copy-Item (Join-Path $Dest "defjam_recomp\LICENSE") (Join-Path $Dest "LICENSE")
if ($Complet) { Copy-Item -Recurse $OutilsSrc (Join-Path $Dest "outils") }
@(
    "$Nom",
    "paquet fabrique le $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
    "xboxrecomp    $cTool ($ToolkitRef)",
    "defjam_recomp $cPort ($PortRef)"
) -join "`r`n" | Set-Content -Path (Join-Path $Dest "VERSION.txt") -Encoding ASCII

Compress-Archive -Path $Dest -DestinationPath $Zip
$n = (Get-ChildItem $Dest -Recurse -File).Count
$mo = (Get-ChildItem $Dest -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("{0} : {1} fichiers, {2:N1} Mo ; zip {3:N1} Mo" -f $Dest, $n, $mo, ((Get-Item $Zip).Length / 1MB)) -ForegroundColor Green
Write-Host "  xboxrecomp $cTool, defjam_recomp $cPort"

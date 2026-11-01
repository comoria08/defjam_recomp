# fabriquer_release.ps1 - les fichiers d'une release, dans paquet\sortie\release_<version>\ :
#
#   DJFFNY.exe                         le lanceur (build_lanceur\Release, a compiler avant)
#   DJFFNY-Recomp-MSVC-DLSS.zip        le paquet leger (portage + toolkit)
#   DJFFNY-Recomp-Clang-complet.zip    le meme, avec les outils (Clang, Python, CMake)
#   versions.json                      ce que DJFFNY.exe lit pour se mettre a jour :
#       lanceur    la version de DJFFNY.exe (s'il est plus recent, le lanceur se remplace)
#       jeu        l'empreinte du portage + toolkit (si elle change, mise a jour incrementale)
#       outils     l'empreinte des outils du paquet complet (si elle change, ils sont retelecharges)
#
# Ne publie rien. Les noms des fichiers sont ceux que DJFFNY.exe telecharge : ne pas les changer.
#
# Usage : .\paquet\fabriquer_release.ps1 -Version 1.2.0 [-ToolkitRef publication-toolkit] [-PortRef HEAD]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ToolkitRef = "publication-toolkit",
    [string]$PortRef = "HEAD"
)
$ErrorActionPreference = "Stop"
$Projet = Split-Path -Parent $PSScriptRoot
$Sortie = Join-Path $PSScriptRoot "sortie"
$Rel = Join-Path $Sortie "release_$Version"
$Exe = Join-Path $Projet "build_lanceur\Release\DJFFNY.exe"
if (-not (Test-Path $Exe)) { throw "DJFFNY.exe introuvable : compiler lanceur\ d'abord (voir lanceur\CMakeLists.txt)" }
$v = (Get-Item $Exe).VersionInfo.ProductVersion
if ($v -ne $Version) { throw "DJFFNY.exe est en version $v, pas $Version (lanceur\djffny.rc et VERSION_LANCEUR)" }

& (Join-Path $PSScriptRoot "fabriquer_paquet.ps1") -ToolkitRef $ToolkitRef -PortRef $PortRef
& (Join-Path $PSScriptRoot "fabriquer_paquet.ps1") -ToolkitRef $ToolkitRef -PortRef $PortRef -Complet
$leger = Get-Content (Join-Path $Sortie "DJFFNY-Recomp\manifest.json") -Raw | ConvertFrom-Json
$complet = Get-Content (Join-Path $Sortie "DJFFNY-Recomp-complet\manifest.json") -Raw | ConvertFrom-Json
if ($leger.empreinte -ne $complet.empreinte) { throw "les deux paquets n'ont pas le meme portage" }

if (Test-Path $Rel) { Remove-Item -Recurse -Force $Rel }
New-Item -ItemType Directory -Force $Rel | Out-Null
Copy-Item $Exe (Join-Path $Rel "DJFFNY.exe")
Copy-Item (Join-Path $Sortie "DJFFNY-Recomp.zip") (Join-Path $Rel "DJFFNY-Recomp-MSVC-DLSS.zip")
Copy-Item (Join-Path $Sortie "DJFFNY-Recomp-complet.zip") (Join-Path $Rel "DJFFNY-Recomp-Clang-complet.zip")
$versions = [ordered]@{ lanceur = $Version; jeu = $leger.empreinte; outils = $complet.outils }
[IO.File]::WriteAllText((Join-Path $Rel "versions.json"), ($versions | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
Get-ChildItem $Rel | ForEach-Object { "{0,-34} {1,12:N0}" -f $_.Name, $_.Length }

# installer.ps1 - Def Jam: Fight for NY, recompilation statique : de l'ISO au jeu
# (points 142 et 143).
#
# Aucune donnee du jeu, aucun code tire du jeu n'est distribue : c'est cette
# machine qui refait tout, a partir de l'image disque du joueur.
#
#   1. verifie les outils (Visual Studio C++, Python + capstone, CMake) ; s'il en
#      manque, propose de les installer par winget, et ne le fait qu'avec
#      l'accord du joueur (ou -Oui) ;
#   2. lit default.xbe dans l'ISO et verifie son empreinte : seule la version
#      prise en charge est acceptee (une autre version a d'autres adresses) ;
#   3. extrait le disque dans game_files\ ;
#   4. refait tout le pipeline (analyse du XBE, desassemblage avec les graines
#      du portage, identification, ABI, traduction en C) dans pipeline\ et
#      src\recomp\gen\, sans toucher aux dossiers de sortie du toolkit ;
#   5. compile l'executable ;
#   6. prepare le dossier des mods et, avec -Raccourci, un raccourci pour jouer.
#
# Les messages sont en francais si Windows l'est, en anglais sinon (-Langue fr|en
# pour forcer). Le fichier est en UTF-8 avec BOM : Windows PowerShell 5.1 lit
# sinon les accents comme de l'ANSI.
#
# Usage (PowerShell, depuis n'importe ou) :
#   .\installeur\installer.ps1 -Iso "D:\jeux\DefJam.iso"
#   .\installeur\installer.ps1 -Xbe "D:\...\default.xbe"      # developpement
#
# -Projet     dossier du projet (defaut : le parent de installeur\)
# -Toolkit    dossier du toolkit xboxrecomp (defaut : ..\xboxrecomp a cote du projet)
# -Oui        installe les outils manquants sans poser la question
# -Raccourci  dossier ou poser le raccourci du jeu (le paquet public le passe)
# -Outils     les outils embarques du paquet complet (defaut : ..\outils a cote
#             du projet). S'ils sont la -- Clang (llvm-mingw), Python et CMake --
#             ils servent a tout et rien n'est a installer (point 145).
# Tout est ecrit dans <Projet>\installation.log.
param(
    [string]$Iso = "",
    [string]$Xbe = "",
    [Alias("Project")][string]$Projet = "",
    [string]$Toolkit = "",
    [Alias("NoBuild")][switch]$SansCompilation,
    [Alias("Yes")][switch]$Oui,
    [Alias("Shortcut")][string]$Raccourci = "",
    [Alias("Language")][string]$Langue = "",
    [Alias("Tools")][string]$Outils = ""
)

# "Continue" et non "Stop" : Windows PowerShell 5.1 fait une erreur de chaque
# ligne que Python ecrit sur stderr (sa progression). Les vrais echecs se lisent
# dans les codes de sortie.
$ErrorActionPreference = "Continue"

if (-not $Langue) { $Langue = (Get-UICulture).TwoLetterISOLanguageName }
$Francais = $Langue -eq "fr"
function L($f, $e) { if ($Francais) { $f } else { $e } }

# La version prise en charge : Def Jam: Fight for NY, Xbox, USA, titre 0x45410049,
# XDK 5849, construit le 2004-08-18. Toute autre version a d'autres adresses et
# les graines, les remplacements manuels et les correctifs ne lui iraient pas.
$XbeSha1 = "4352DC8438BCE03E792FAB51E5CBF24EF492F770"
# Les autres versions deja vues, pour dire au joueur laquelle il a donnee.
$Connues = @{
    "5D2A818CCCD7829E2E17FABA9962279CA547FCE5" = @("la version Europe (En,Fr)", "the Europe (En,Fr) version")
}
function Refuser($quoi, $h) {
    if ($Connues.ContainsKey($h)) {
        $v = $Connues[$h]
        $laquelle = L "c'est $($v[0]) du jeu" "it is $($v[1]) of the game"
    } else {
        $laquelle = L "version inconnue" "unknown version"
    }
    Echec (L "$quoi n'est pas la version prise en charge : $laquelle. Seule la version USA l'est (empreinte du XBE $h, attendue $XbeSha1)." `
             "$quoi is not the supported version: $laquelle. Only the USA version is (XBE fingerprint $h, expected $XbeSha1).")
}

if (-not $Projet)  { $Projet = Split-Path -Parent $PSScriptRoot }
$Projet = (Resolve-Path $Projet).Path
if (-not $Toolkit) { $Toolkit = Join-Path (Split-Path -Parent $Projet) "xboxrecomp" }
$Log     = Join-Path $Projet "installation.log"
$Game    = Join-Path $Projet "game_files"
$XbeDest = Join-Path $Game "default.xbe"
$Pipe    = Join-Path $Projet "pipeline"
$Gen     = Join-Path $Projet "src\recomp\gen"
$debut   = Get-Date

function Ecrire($msg, $couleur = "Gray") {
    Write-Host $msg -ForegroundColor $couleur
    Add-Content -Path $Log -Value $msg -Encoding UTF8
}
function Etape($n, $msg) { Ecrire "`n=== [$n] $msg ===" "Cyan" }
function Echec($msg) {
    Ecrire ("`n" + (L "ÉCHEC : " "FAILED: ") + $msg) "Red"
    Ecrire (L "Le détail est dans $Log." "Details are in $Log.") "Red"
    exit 1
}
# Une commande externe, sa sortie ajoutee au journal, son code verifie.
function Lancer($quoi, $exe, [string[]]$arguments) {
    Ecrire "  > $exe $($arguments -join ' ')" "DarkGray"
    & $exe @arguments 2>&1 | ForEach-Object { Add-Content -Path $Log -Value "$_" -Encoding UTF8 }
    if ($LASTEXITCODE -ne 0) { Echec ((L "$quoi (code $LASTEXITCODE)" "$quoi (exit code $LASTEXITCODE)")) }
}
function Sha1($chemin) {
    return (Get-FileHash -Algorithm SHA1 -Path $chemin).Hash.ToUpper()
}

Set-Content -Path $Log -Value "Installation $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -Encoding UTF8
Ecrire (L "Def Jam: Fight for NY - recompilation statique" "Def Jam: Fight for NY - static recompilation") "White"
Ecrire ((L "  projet  : " "  project : ") + $Projet)
Ecrire "  toolkit : $Toolkit"

# ── 1. Les outils ──────────────────────────────────────────────────────

# Un programme qu'on vient d'installer n'est pas dans le PATH de cette session :
# on le relit dans le registre.
function Relire-Path {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}
function Trouver-Python {
    $script:py = $null; $script:pyArgs = @()
    if (Get-Command py -ErrorAction SilentlyContinue) { $script:py = "py"; $script:pyArgs = @("-3"); return }
    # Le "python" de WindowsApps n'est qu'un raccourci vers le Microsoft Store.
    $c = Get-Command python -ErrorAction SilentlyContinue
    if ($c -and $c.Source -notmatch "WindowsApps") { $script:py = $c.Source; return }
    # Installe pour l'utilisateur sans l'ajouter au PATH : ses dossiers par defaut.
    foreach ($base in @("$env:LOCALAPPDATA\Programs\Python", $env:ProgramFiles)) {
        $p = Get-ChildItem -Path $base -Filter "Python3*" -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName "python.exe" } |
             Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($p) { $script:py = $p; return }
    }
}
function Trouver-Outils {
    Trouver-Python
    $script:capstone = $false
    if ($script:py) {
        & $script:py @script:pyArgs -c "import capstone" 2>$null
        $script:capstone = ($LASTEXITCODE -eq 0)
    }
    $script:vs = $null; $script:vsPath = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $req = @("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
        $script:vs     = & $vswhere @req -property displayName
        $script:vsPath = & $vswhere @req -property installationPath
    }
    $script:cmakeExe = $null
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { $script:cmakeExe = $c.Source }
    elseif ($script:vsPath) {
        # Les outils C++ de Visual Studio apportent leur propre CMake.
        $p = Join-Path $script:vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $p) { $script:cmakeExe = $p }
    }
}
function Manques {
    $m = @()
    if (-not $script:vs)       { $m += "vs" }
    if (-not $script:py)       { $m += "python" }
    if (-not $script:cmakeExe) { $m += "cmake" }
    if (-not $script:capstone) { $m += "capstone" }
    return $m
}
$Noms = @{
    vs       = "Visual Studio Build Tools (C++)"
    python   = "Python 3"
    cmake    = "CMake"
    capstone = (L "le module Python capstone" "the capstone Python module")
}
$Liens = @{
    vs       = "https://visualstudio.microsoft.com/visual-cpp-build-tools/  (winget install Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait`")"
    python   = "https://www.python.org/downloads/  (winget install Python.Python.3.12)"
    cmake    = "https://cmake.org/download/  (winget install Kitware.CMake)"
    capstone = "py -3 -m pip install capstone"
}
function Installer-Outil($quoi) {
    $accord = @("--accept-package-agreements", "--accept-source-agreements")
    switch ($quoi) {
        "vs" {
            Ecrire (L "  Visual Studio Build Tools : 10 à 20 minutes, Windows va demander les droits administrateur..." `
                      "  Visual Studio Build Tools: 10 to 20 minutes, Windows will ask for administrator rights...") "Yellow"
            & winget install -e --id Microsoft.VisualStudio.2022.BuildTools @accord `
                --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait"
        }
        "python" { & winget install -e --id Python.Python.3.12 --silent @accord }
        "cmake"  { & winget install -e --id Kitware.CMake --silent @accord }
        "capstone" {
            Trouver-Python
            if ($script:py) { & $script:py @script:pyArgs -m pip install capstone }
        }
    }
    Ecrire "  winget/pip : code $LASTEXITCODE" "DarkGray"
    Relire-Path
    Trouver-Outils
}

Etape 1 (L "Outils" "Tools")
if (-not $Outils) { $Outils = Join-Path (Split-Path -Parent $Projet) "outils" }
$Embarque = Test-Path (Join-Path $Outils "llvm-mingw\bin\clang.exe")
if ($Embarque) {
    # Le paquet complet : ses outils, et aucun de ceux de la machine.
    $Outils = (Resolve-Path $Outils).Path
    $script:py = Join-Path $Outils "python\python.exe"; $script:pyArgs = @()
    $script:cmakeExe = Join-Path $Outils "cmake\bin\cmake.exe"
    $script:vs = "Clang (llvm-mingw, " + (L "embarqué" "embedded") + ")"
    $clangBin = Join-Path $Outils "llvm-mingw\bin"
    $env:Path = "$clangBin;" + $env:Path
    # Le Python embarquable ne lit ni PYTHONPATH ni le dossier courant : le
    # toolkit doit etre dans son fichier ._pth. Celui du paquet y est deja
    # (..\..\xboxrecomp) ; un -Toolkit ailleurs y est ajoute.
    $pth = Get-ChildItem (Join-Path $Outils "python") -Filter "python*._pth" | Select-Object -First 1
    if ($pth -and -not ((Get-Content $pth.FullName) -contains $Toolkit)) {
        Add-Content -Path $pth.FullName -Value $Toolkit -Encoding ASCII
    }
    & $script:py -c "import capstone" 2>$null
    if ($LASTEXITCODE -ne 0) { Echec (L "le Python embarqué ne trouve pas capstone ($Outils)" "the embedded Python cannot find capstone ($Outils)") }
    foreach ($f in @($script:cmakeExe, (Join-Path $clangBin "mingw32-make.exe"))) {
        if (-not (Test-Path $f)) { Echec ((L "outil embarqué absent : " "embedded tool missing: ") + $f) }
    }
} else {
Trouver-Outils
$manque = @(Manques)
}
if (-not $Embarque -and $manque.Count) {
    Ecrire (L "`nIl manque :" "`nMissing:") "Yellow"
    foreach ($m in $manque) { Ecrire "  - $($Noms[$m])" "Yellow" }
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if ($winget) {
        $ok = [bool]$Oui
        if (-not $ok) {
            Ecrire (L "`nLes installer maintenant avec winget ? Leurs licences seront acceptées en votre nom. Visual Studio Build Tools demande les droits administrateur, quelques Go et 10 à 20 minutes." `
                      "`nInstall them now with winget? Their licences will be accepted on your behalf. Visual Studio Build Tools needs administrator rights, a few GB and 10 to 20 minutes.") "White"
            $r = Read-Host (L "[O]ui / [N]on" "[Y]es / [N]o")
            Add-Content -Path $Log -Value "  > $r" -Encoding UTF8
            $ok = $r -match '^\s*[oOyY]'
        }
        if ($ok) {
            # Visual Studio d'abord : il apporte peut-etre CMake.
            foreach ($m in @("vs", "python", "cmake", "capstone")) {
                if ((Manques) -contains $m) {
                    Ecrire ((L "  installation : " "  installing: ") + $Noms[$m])
                    Installer-Outil $m
                }
            }
        }
    }
    $manque = @(Manques)
    if ($manque.Count) {
        Ecrire (L "`nÀ installer à la main, puis relancer l'installation :" "`nInstall by hand, then run the installation again:") "Yellow"
        foreach ($m in $manque) { Ecrire "  - $($Noms[$m]) : $($Liens[$m])" "Yellow" }
        Echec (L "outils manquants" "missing tools")
    }
}
Ecrire ((L "  Compilateur : " "  Compiler: ") + $script:vs)
Ecrire "  Python : $($script:py) $($script:pyArgs -join ' ') (capstone OK)"
Ecrire "  CMake : $($script:cmakeExe)"
$py = $script:py; $pyArgs = $script:pyArgs
if (-not (Test-Path (Join-Path $Toolkit "tools\recomp"))) {
    Echec (L "le toolkit xboxrecomp est introuvable dans $Toolkit" "the xboxrecomp toolkit was not found in $Toolkit")
}

# ── 2 et 3. Le jeu ─────────────────────────────────────────────────────
Etape 2 (L "Le jeu" "The game")
New-Item -ItemType Directory -Force $Game | Out-Null
if (-not $Iso -and -not $Xbe -and -not (Test-Path $XbeDest)) {
    # Lance d'un double-clic : on demande l'image disque.
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = L "Image disque Xbox de Def Jam: Fight for NY (USA)" "Xbox disc image of Def Jam: Fight for NY (USA)"
    $dlg.Filter = (L "Image disque Xbox" "Xbox disc image") + " (*.iso;*.xiso)|*.iso;*.xiso|" + (L "Tous les fichiers" "All files") + " (*.*)|*.*"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Iso = $dlg.FileName }
}
if ($Iso) {
    if (-not (Test-Path $Iso)) { Echec ((L "ISO introuvable : " "ISO not found: ") + $Iso) }
    $tmp = Join-Path $Pipe "iso_xbe"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    Set-Location $Toolkit
    Lancer (L "lecture de default.xbe dans l'ISO" "reading default.xbe from the ISO") $py ($pyArgs + @("-m", "tools.xiso", "get", $Iso, "default.xbe", "-o", $tmp))
    $h = Sha1 (Join-Path $tmp "default.xbe")
    if ($h -ne $XbeSha1) { Refuser (L "cette ISO" "this ISO") $h }
    Ecrire ((L "  version reconnue (empreinte " "  version recognised (fingerprint ") + "$h)") "Green"
    Etape 3 (L "Extraction du disque (quelques minutes)" "Extracting the disc (a few minutes)")
    Lancer (L "extraction de l'ISO" "extracting the ISO") $py ($pyArgs + @("-m", "tools.xiso", "unpack", $Iso, "-o", $Game))
} elseif ($Xbe) {
    if (-not (Test-Path $Xbe)) { Echec ((L "XBE introuvable : " "XBE not found: ") + $Xbe) }
    $h = Sha1 $Xbe
    if ($h -ne $XbeSha1) { Refuser (L "ce XBE" "this XBE") $h }
    if ((Resolve-Path $Xbe).Path -ne $XbeDest) { Copy-Item $Xbe $XbeDest -Force }
    Ecrire ((L "  XBE reconnu (empreinte $h) ; pas d'extraction : développement" "  XBE recognised (fingerprint $h); no extraction: development")) "Green"
} elseif (Test-Path $XbeDest) {
    $h = Sha1 $XbeDest
    if ($h -ne $XbeSha1) { Refuser "game_files\default.xbe" $h }
    Ecrire ((L "  game_files déjà en place (empreinte $h)" "  game_files already in place (fingerprint $h)")) "Green"
} else {
    Echec (L "donner l'image disque du jeu : -Iso `"chemin\vers\le\jeu.iso`"" "give the game's disc image: -Iso `"path\to\the\game.iso`"")
}

# ── 4. Le pipeline ─────────────────────────────────────────────────────
Etape 4 (L "Recompilation vers C (quelques minutes)" "Recompiling to C (a few minutes)")
$dis = Join-Path $Pipe "disasm"; $fid = Join-Path $Pipe "func_id"
$abi = Join-Path $Pipe "abi";    $rec = Join-Path $Pipe "recomp"
foreach ($d in @($dis, $fid, $abi, $rec, $Gen)) { New-Item -ItemType Directory -Force $d | Out-Null }
$analyse = Join-Path $Game "default_analysis.json"
Set-Location $Toolkit
Ecrire (L "  analyse du XBE" "  XBE analysis")
Lancer (L "analyse du XBE" "XBE analysis") $py ($pyArgs + @("-m", "tools.xbe_parser", $XbeDest, "--json", $analyse))
# Les graines du portage, dans l'ordre de feedback_loop.ps1 : cibles d'appels
# indirects mesurees en jeu, pilotes XPP, table de l'interpreteur, divers.
$graines = @()
foreach ($f in @("icall_seeds.json", "xpp_seeds.json", "interp_seeds.json", "misc_seeds.json")) {
    $p = Join-Path $Projet $f
    if (Test-Path $p) { $graines += @("--seed-functions", $p) }
    else { Echec ((L "graines absentes : " "seed file missing: ") + $p) }
}
Ecrire (L "  désassemblage" "  disassembly")
Lancer (L "désassemblage" "disassembly") $py ($pyArgs + @("-m", "tools.disasm", $XbeDest, "-o", $dis, "--analysis-json", $analyse, "--force") + $graines)
Ecrire (L "  identification des fonctions" "  function identification")
Lancer (L "identification" "identification") $py ($pyArgs + @("-m", "tools.func_id", $XbeDest, "--functions", (Join-Path $dis "functions.json"),
    "--strings", (Join-Path $dis "strings.json"), "--xrefs", (Join-Path $dis "xrefs.json"), "-o", $fid))
Ecrire (L "  conventions d'appel" "  calling conventions")
Lancer (L "analyse ABI" "ABI analysis") $py ($pyArgs + @("-m", "tools.abi_analysis", $XbeDest, "--disasm-dir", $dis, "--func-id-dir", $fid, "--output-dir", $abi))
Ecrire (L "  traduction en C" "  translation to C")
Get-ChildItem $Gen -Include recomp_0*.c, recomp_dispatch.c, recomp_stubs_unresolved.c, recomp_funcs.h -Recurse -ErrorAction SilentlyContinue | Remove-Item
Lancer (L "traduction en C" "translation to C") $py ($pyArgs + @("-m", "tools.recomp", $XbeDest, "--all", "--split", "1000", "--game-name", "DefJam",
    "--gen-dir", $Gen, "--exclude-manual", (Join-Path $Projet "src\recomp_manual.c"),
    "--disasm-dir", $dis, "--func-id-dir", $fid, "--abi-dir", $abi, "-o", $rec))
if (-not (Test-Path (Join-Path $Gen "recomp_types.h"))) {
    Echec (L "la traduction n'a pas produit recomp_types.h" "the translation did not produce recomp_types.h")
}
$n = (Get-ChildItem $Gen -Filter "recomp_0*.c").Count
Ecrire ((L "  $n fichiers de C générés dans src\recomp\gen" "  $n C files generated in src\recomp\gen")) "Green"

# ── 5. La compilation ──────────────────────────────────────────────────
$exe = Join-Path $Projet "build\Release\defjam_recomp.exe"
if ($SansCompilation) {
    Ecrire (L "`nCompilation sautée (-SansCompilation)." "`nBuild skipped (-NoBuild).") "Yellow"
} else {
    Etape 5 (L "Compilation (quelques minutes)" "Build (a few minutes)")
    $build = Join-Path $Projet "build"
    # Un build\ configure pour l'autre compilateur ne se reconfigure pas : CMake
    # refuse de changer de generateur. On le recommence.
    $cache = Join-Path $build "CMakeCache.txt"
    $voulu = if ($Embarque) { "MinGW Makefiles" } else { "Visual Studio" }
    if ((Test-Path $cache) -and -not (Select-String -Path $cache -SimpleMatch "CMAKE_GENERATOR:INTERNAL=$voulu" -Quiet)) {
        Remove-Item -Recurse -Force $build
    }
    if ($Embarque) {
        # Des chemins avec / : CMake lit \U d'un chemin Windows comme un echappement.
        function Cm($p) { return ($p -replace '\\', '/') }
        Lancer (L "configuration CMake" "CMake configuration") $script:cmakeExe @(
            "-S", $Projet, "-B", $build, "-G", "MinGW Makefiles", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=$(Cm (Join-Path $clangBin 'clang.exe'))",
            "-DCMAKE_RC_COMPILER=$(Cm (Join-Path $clangBin 'llvm-rc.exe'))",
            "-DCMAKE_MAKE_PROGRAM=$(Cm (Join-Path $clangBin 'mingw32-make.exe'))",
            # build\Release\, comme avec Visual Studio : le lanceur y cherche le jeu.
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(Cm (Join-Path $build 'Release'))",
            "-DXBOXRECOMP_DIR=$(Cm $Toolkit)")
        Lancer (L "compilation" "build") $script:cmakeExe @("--build", $build, "--parallel", "$env:NUMBER_OF_PROCESSORS")
    } else {
        Lancer (L "configuration CMake" "CMake configuration") $script:cmakeExe @("-S", $Projet, "-B", $build, "-DXBOXRECOMP_DIR=$Toolkit")
        Lancer (L "compilation" "build") $script:cmakeExe @("--build", $build, "--config", "Release", "--parallel")
    }
    if (-not (Test-Path $exe)) { Echec (L "l'exécutable n'a pas été produit" "the executable was not produced") }
    Ecrire "  $exe" "Green"
}

# ── 6. Finitions ───────────────────────────────────────────────────────
Etape 6 (L "Finitions" "Finishing")
New-Item -ItemType Directory -Force (Join-Path $Game "mods") | Out-Null
Ecrire (L "  dossier des mods : game_files\mods" "  mods folder: game_files\mods")
$lnk = $null
if ($Raccourci -and -not $SansCompilation) {
    $lnk = Join-Path (Resolve-Path $Raccourci).Path "Def Jam Fight for NY.lnk"
    $s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $s.TargetPath = $exe
    $s.WorkingDirectory = $Projet
    $s.Save()
    Ecrire ((L "  raccourci : " "  shortcut: ") + $lnk)
}
$duree = (Get-Date) - $debut
Ecrire ((L "`nInstallation terminée en {0:mm} min {0:ss} s." "`nInstallation finished in {0:mm} min {0:ss} s.") -f $duree) "Green"
if (-not $SansCompilation) {
    if ($lnk) { Ecrire (L "Pour jouer : double-cliquer « Def Jam Fight for NY »." 'To play: double-click "Def Jam Fight for NY".') "Green" }
    else      { Ecrire (L "Pour jouer : double-cliquer build\Release\defjam_recomp.exe." "To play: double-click build\Release\defjam_recomp.exe.") "Green" }
}

# preparer_outils.ps1 - les outils de compilation embarques du paquet complet (point 145).
#
# Un joueur n'a alors rien a installer : ni Visual Studio, ni Python, ni CMake.
# Tout est redistribuable, et chaque licence est gardee a cote de son outil :
#   llvm-mingw   Clang + LLD + en-tetes mingw-w64   Apache-2.0 avec exception LLVM,
#                                                    mingw-w64 (ZPL, domaine public, MIT...)
#   Python       l'embarquable officiel               PSF
#   capstone     la roue win_amd64 (ctypes + DLL)     BSD
#   CMake        l'archive portable                   BSD-3
#
# Les versions et les empreintes sont figees ici : le dossier obtenu est le meme
# partout. Les archives sont telechargees dans -Cache si elles n'y sont pas, et
# toujours verifiees.
#
# Usage : .\paquet\preparer_outils.ps1 [-Dossier paquet\outils] [-Cache paquet\telechargements]
param(
    [string]$Dossier = "",
    [string]$Cache = ""
)
$ErrorActionPreference = "Stop"
if (-not $Dossier) { $Dossier = Join-Path $PSScriptRoot "outils" }
if (-not $Cache)   { $Cache   = Join-Path $PSScriptRoot "telechargements" }

$Archives = @(
    @{ Nom = "llvm-mingw-20260922-ucrt-x86_64.zip"
       Url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip"
       Sha = "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666" },
    @{ Nom = "python-3.14.7-embed-amd64.zip"
       Url = "https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip"
       Sha = "d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15" },
    @{ Nom = "capstone-5.0.9-py3-none-win_amd64.whl"
       Url = "https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl"
       Sha = "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0" },
    @{ Nom = "cmake-4.4.3-windows-x86_64.zip"
       Url = "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip"
       Sha = "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab" }
)

$tar = "$env:SystemRoot\System32\tar.exe"   # celui de Git lirait "C:" comme un hote
New-Item -ItemType Directory -Force $Cache | Out-Null
foreach ($a in $Archives) {
    $f = Join-Path $Cache $a.Nom
    if (-not (Test-Path $f)) {
        Write-Host "telechargement : $($a.Nom)"
        & curl.exe -sSL -o $f $a.Url
        if ($LASTEXITCODE -ne 0) { throw "telechargement echoue : $($a.Url)" }
    }
    $h = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
    if ($h -ne $a.Sha) { throw "$($a.Nom) : empreinte $h, attendue $($a.Sha)" }
    Write-Host "  $($a.Nom) : empreinte verifiee"
}

if (Test-Path $Dossier) { Remove-Item -Recurse -Force $Dossier }
New-Item -ItemType Directory -Force $Dossier | Out-Null
$tmp = Join-Path $Dossier "_tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null

# ── llvm-mingw : la seule cible x86_64, sans debogueur ni outils d'analyse ──
& $tar -x -f (Join-Path $Cache $Archives[0].Nom) -C $tmp
$src = Join-Path $tmp "llvm-mingw-20260922-ucrt-x86_64"
$dst = Join-Path $Dossier "llvm-mingw"
New-Item -ItemType Directory -Force (Join-Path $dst "bin") | Out-Null
foreach ($d in @("include", "lib", "x86_64-w64-mingw32", "share")) {
    Copy-Item -Recurse (Join-Path $src $d) (Join-Path $dst $d)
}
Copy-Item (Join-Path $src "LICENSE.TXT") $dst
# Ce que CMake et la compilation appellent, et les DLL dont ces outils dependent
# (lues avec llvm-objdump -p).
# clang.exe n'est qu'un lanceur (clang-target-wrapper) : il appelle clang-23.exe
# avec la configuration de la cible, les fichiers x86_64-*.cfg.
$garder = @(
    "clang.exe", "clang++.exe", "clang-23.exe", "clang-target-wrapper.exe", "ld.lld.exe",
    "llvm-ar.exe", "llvm-ranlib.exe", "llvm-rc.exe", "llvm-windres.exe", "llvm-objcopy.exe",
    "llvm-strip.exe", "llvm-nm.exe", "llvm-dlltool.exe", "mingw32-make.exe",
    "mingw32-common.cfg",   # inclus par les x86_64-*.cfg (@mingw32-common.cfg)
    "libLLVM-23.dll", "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll"
)
Get-ChildItem (Join-Path $src "bin") | Where-Object {
    $garder -contains $_.Name -or $_.Name -like "x86_64-*"
} | ForEach-Object { Copy-Item $_.FullName (Join-Path $dst "bin") }
# Des bibliotheques d'execution de Clang, seules les builtins x86_64 de Windows
# servent : ni Linux, ni les autres architectures, ni sanitizers ni fuzzer.
Get-ChildItem (Join-Path $dst "lib\clang") -Recurse -Directory -Filter "linux" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
Get-ChildItem (Join-Path $dst "lib\clang") -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -like "*\lib\windows" -and $_.Name -notlike "*builtins-x86_64*" } |
    Remove-Item

# ── Python embarquable + capstone ─────────────────────────────────────
$py = Join-Path $Dossier "python"
New-Item -ItemType Directory -Force $py | Out-Null
& $tar -x -f (Join-Path $Cache $Archives[1].Nom) -C $py
$sp = Join-Path $py "Lib\site-packages"
New-Item -ItemType Directory -Force $sp | Out-Null
& $tar -x -f (Join-Path $Cache $Archives[2].Nom) -C $sp     # une roue est un zip
# L'embarquable ignore PYTHONPATH et le dossier courant : ses chemins sont dans
# son fichier ._pth, relatifs a lui. Le toolkit est a ..\..\xboxrecomp dans le
# paquet ; l'installeur ajoute aussi son chemin reel.
$pth = Get-ChildItem $py -Filter "python*._pth" | Select-Object -First 1
Add-Content -Path $pth.FullName -Value "Lib\site-packages" -Encoding ASCII
Add-Content -Path $pth.FullName -Value "..\..\xboxrecomp" -Encoding ASCII

# ── CMake portable, sans aide ni interface graphique ─────────────────
& $tar -x -f (Join-Path $Cache $Archives[3].Nom) -C $tmp
$csrc = Join-Path $tmp "cmake-4.4.3-windows-x86_64"
$cm = Join-Path $Dossier "cmake"
New-Item -ItemType Directory -Force (Join-Path $cm "bin"), (Join-Path $cm "share") | Out-Null
Copy-Item (Join-Path $csrc "bin\cmake.exe") (Join-Path $cm "bin")
Copy-Item -Recurse (Join-Path $csrc "share\cmake-4.4") (Join-Path $cm "share\cmake-4.4")
Remove-Item -Recurse -Force (Join-Path $cm "share\cmake-4.4\Help") -ErrorAction SilentlyContinue
# Sa licence, et celles des composants qu'il embarque (curl, zlib, libarchive...),
# sans la documentation elle-meme.
$lic = Join-Path $cm "licences"
Copy-Item -Recurse (Join-Path $csrc "doc\cmake") $lic
Remove-Item -Recurse -Force (Join-Path $lic "html"), (Join-Path $lic "CMake.qch") -ErrorAction SilentlyContinue

Remove-Item -Recurse -Force $tmp
@(
    "Outils de compilation embarques (paquet complet) - voir paquet\preparer_outils.ps1",
    "",
    "llvm-mingw 20260922 ucrt x86_64   https://github.com/mstorsjo/llvm-mingw   llvm-mingw\LICENSE.TXT",
    "Python 3.14.7 embarquable         https://www.python.org                   python\LICENSE.txt",
    "capstone 5.0.9                    https://www.capstone-engine.org          python\Lib\site-packages\capstone-5.0.9.dist-info",
    "CMake 4.4.3                       https://cmake.org                        cmake\licences\LICENSE.rst"
) -join "`r`n" | Set-Content -Path (Join-Path $Dossier "OUTILS.txt") -Encoding ASCII

$mo = (Get-ChildItem $Dossier -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("outils prets : {0:N0} Mo dans {1}" -f $mo, $Dossier) -ForegroundColor Green

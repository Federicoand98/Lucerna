# Scarica gli asset grandi che non stanno nel repo (ADR 0022): l'equivalente Windows di
# fetch_assets.sh. Si lancia una volta dopo il clone:
#   tools\fetch_assets.ps1 [cartella di destinazione di Sponza]
#
# Sponza: la versione corretta di jimmiebergmann/Sponza (Crytek Sponza di Frank Meinl, CC BY 3.0),
# fissata a un commit e verificata: la scena integrata 08-sponza conta su questi file esatti.
# Usa curl.exe e tar.exe, che Windows 10 e 11 hanno di serie.
param([string]$Dest = (Join-Path $PSScriptRoot "..\res\models\Sponza"))
$ErrorActionPreference = "Stop"
$Dest = [System.IO.Path]::GetFullPath($Dest)

$commit = "222338979d32f4f4818466291bdbc29f192b86ba"
$objSha256 = "ec7b65b793692db211e3011d71f8b02f3d3b57ec5c7da90e5805360ce44f452b"

if (Test-Path (Join-Path $Dest "sponza.obj")) {
    Write-Host "Sponza is already in $Dest"
    exit 0
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("lucerna-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Write-Host "Downloading Sponza (about 110 MB)..."
    $archive = Join-Path $tmp "sponza.tar.gz"
    & curl.exe -fL --progress-bar -o $archive "https://github.com/jimmiebergmann/Sponza/archive/$commit.tar.gz"
    if ($LASTEXITCODE -ne 0) { throw "Download failed" }
    & tar.exe -xzf $archive -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed" }

    $src = Join-Path $tmp "Sponza-$commit"
    $hash = (Get-FileHash (Join-Path $src "sponza.obj") -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $objSha256) { throw "sponza.obj checksum mismatch: $hash" }

    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Copy-Item (Join-Path $src "sponza.obj"), (Join-Path $src "sponza.mtl") -Destination $Dest
    Copy-Item (Join-Path $src "textures") -Destination $Dest -Recurse -Force
    Write-Host "Sponza installed in $Dest"
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

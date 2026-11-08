# Scarica gli asset grandi che non stanno nel repo (ADR 0022): l'equivalente Windows di
# fetch_assets.sh, che elenca gli asset e le scene che li usano. Si lancia dopo il clone:
#   tools\fetch_assets.ps1 [asset...]      (sponza helmet spheres breakfast bistro zeroday)
# Senza argomenti li scarica tutti; un asset gia' presente si salta. Zero-Day si converte con
# Blender ($env:BLENDER, il PATH, Program Files o una libreria Steam) e Python con Pillow e numpy
# ($env:PYTHON o python dal PATH). Usa curl.exe e tar.exe, che Windows 10 e 11 hanno di serie.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Assets)
$ErrorActionPreference = "Stop"

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$models = Join-Path $root "res\models"
$all = @("sponza", "helmet", "spheres", "breakfast", "bistro", "zeroday")
if (-not $Assets) { $Assets = $all }

function Download([string]$Url, [string]$File, [string]$Sha256) {
    & curl.exe -fL --progress-bar -o $File $Url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    if ($Sha256) { Assert-Hash $File $Sha256 }
}

function Assert-Hash([string]$File, [string]$Sha256) {
    $hash = (Get-FileHash $File -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $Sha256) { throw "Checksum mismatch for $(Split-Path $File -Leaf): $hash" }
}

function Expand([string]$Archive, [string]$Dest, [string[]]$Members = @()) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    & tar.exe -xf $Archive -C $Dest @Members
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $Archive" }
}

function Fetch-Sponza($tmp) {
    $dest = Join-Path $models "Sponza"
    if (Test-Path (Join-Path $dest "sponza.obj")) { Write-Host "Sponza is already in $dest"; return }
    $commit = "222338979d32f4f4818466291bdbc29f192b86ba"
    Write-Host "Downloading Sponza (about 110 MB)..."
    Download "https://github.com/jimmiebergmann/Sponza/archive/$commit.tar.gz" "$tmp\sponza.tar.gz"
    Expand "$tmp\sponza.tar.gz" $tmp
    $src = Join-Path $tmp "Sponza-$commit"
    Assert-Hash "$src\sponza.obj" "ec7b65b793692db211e3011d71f8b02f3d3b57ec5c7da90e5805360ce44f452b"
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item "$src\sponza.obj", "$src\sponza.mtl" -Destination $dest
    Copy-Item "$src\textures" -Destination $dest -Recurse -Force
    Write-Host "Sponza installed in $dest"
}

# I modelli Khronos sono file singoli, presi a un commit fisso del repository
$khronos = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models"

function Fetch-Khronos($tmp, [string]$Name, [string]$BinSha, [string[]]$Files) {
    $dest = Join-Path $models $Name
    if (Test-Path (Join-Path $dest "$Name.gltf")) { Write-Host "$Name is already in $dest"; return }
    Write-Host "Downloading $Name..."
    $dir = New-Item -ItemType Directory -Force -Path (Join-Path $tmp $Name)
    foreach ($f in $Files + "$Name.gltf") {
        & curl.exe -fsSL -o (Join-Path $dir $f) "$khronos/$Name/glTF/$f"
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $f" }
    }
    & curl.exe -fsSL -o (Join-Path $dir "LICENSE.md") "$khronos/$Name/LICENSE.md"
    Assert-Hash (Get-ChildItem $dir -Filter *.bin | Select-Object -First 1).FullName $BinSha
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item "$dir\*" -Destination $dest
    Write-Host "$Name installed in $dest"
}

function Fetch-Helmet($tmp) {
    Fetch-Khronos $tmp "DamagedHelmet" "61b33ead6aa3de23f39f02aaf0965bc3988575716a768f45b15d5ea3fb783fdf" `
        @("DamagedHelmet.bin", "Default_AO.jpg", "Default_albedo.jpg", "Default_emissive.jpg", "Default_metalRoughness.jpg", "Default_normal.jpg")
}

function Fetch-Spheres($tmp) {
    Fetch-Khronos $tmp "MetalRoughSpheres" "f35a1caf37837b2987e24859d812a23e40ca37e1f741782e2a754961250e8c36" `
        @("MetalRoughSpheres0.bin", "Spheres_BaseColor.png", "Spheres_MetalRough.png")
}

$mcguire = "https://casual-effects.com/g3d/data10/research/model"

function Fetch-Breakfast($tmp) {
    $dest = Join-Path $models "BreakfastRoom"
    if (Test-Path (Join-Path $dest "breakfast_room.obj")) { Write-Host "Breakfast Room is already in $dest"; return }
    Write-Host "Downloading Breakfast Room..."
    Download "$mcguire/breakfast_room/breakfast_room.zip" "$tmp\breakfast_room.zip" "e56ff9b4c89b680cfd5c4c00390533ae28fff7a122d6fc8c92c273a9066c0ca8"
    Expand "$tmp\breakfast_room.zip" $dest
    Write-Host "Breakfast Room installed in $dest"
}

function Fetch-Bistro($tmp) {
    $dest = Join-Path $models "Bistro"
    if (Test-Path (Join-Path $dest "Exterior\exterior.obj")) { Write-Host "Bistro is already in $dest"; return }
    Write-Host "Downloading Bistro (about 1.4 GB)..."
    # Ogni archivio si estrae in una cartella col suo nome: i .mtl puntano a ..\BuildingTextures\...
    $parts = @(
        @("Exterior.zip", "Exterior", "f612ba0d8a65811afe0d48a023d31e5b43ac40b78e9b12d15e2b7dc8923c29d4"),
        @("Interior.zip", "Interior", "3faa41f8d6860d1267268348d44d2b74c64a8bfb55dd4b03c562a70ab83b1062"),
        @("BuildingTextures", "BuildingTextures", "16538a37f34cadec5665d97e5b77ef3581d67ab674b33b9caeef1d277cbde6bf"),
        @("OtherTextures", "OtherTextures", "0806fee3eeb0e14cd501582edbd2d5caf33c5416a62a871e0f126391096f5c58"),
        @("PropTextures", "PropTextures", "a179e4ee6367d2b518c4978d7e2e690c59503861634d67d0da8bacfa7196571a")
    )
    foreach ($p in $parts) {
        $file = Join-Path $tmp $p[0]
        Download "$mcguire/bistro/$($p[0])" $file $p[2]
        Expand $file (Join-Path $dest $p[1])
        Remove-Item $file
    }
    Write-Host "Bistro installed in $dest"
}

function Find-Blender {
    if ($env:BLENDER) { return $env:BLENDER }
    $cmd = Get-Command blender -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(Get-ChildItem "$env:ProgramFiles\Blender Foundation\*\blender.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | ForEach-Object FullName)
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam -and (Test-Path "$steam\steamapps\libraryfolders.vdf")) {
        foreach ($line in Get-Content "$steam\steamapps\libraryfolders.vdf") {
            if ($line -match '"path"\s+"(.+)"') { $candidates += ($Matches[1] -replace '\\\\', '\') + "\steamapps\common\Blender\blender.exe" }
        }
    }
    return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Find-Python {
    $list = @()
    if ($env:PYTHON) { $list += $env:PYTHON }
    $list += "python", "py"
    foreach ($p in $list) {
        if (Get-Command $p -ErrorAction SilentlyContinue) {
            & $p -c "import PIL, numpy" 2>$null
            if ($LASTEXITCODE -eq 0) { return $p }
        }
    }
    return $null
}

function Fetch-Zeroday($tmp) {
    $dest = Join-Path $models "ZeroDay"
    if (Test-Path (Join-Path $dest "MEASURE_SEVEN.gltf")) { Write-Host "Zero-Day is already in $dest"; return }
    $blender = Find-Blender
    if (-not $blender) { throw "Zero-Day skipped: Blender not found (set `$env:BLENDER to blender.exe)" }
    $python = Find-Python
    if (-not $python) { throw "Zero-Day skipped: needs Python with Pillow and numpy (pip install pillow numpy, or set `$env:PYTHON)" }
    Write-Host "Downloading Zero-Day (about 1.1 GB)..."
    Download "https://developer.nvidia.com/beeple" "$tmp\ZeroDay_v1.zip" "a9d040fe9720ee88937778659205fd76bad4499431a37156aaeeba382f08d592"
    $work = Join-Path $tmp "zeroday"
    Expand "$tmp\ZeroDay_v1.zip" $work @("MEASURE_SEVEN", "LICENSE.txt", "README.txt")
    Remove-Item "$tmp\ZeroDay_v1.zip"
    Write-Host "Converting the textures..."
    & $python "$root\tools\zero_day\convert_textures.py" "$work\MEASURE_SEVEN\tex" "$work\png" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Texture conversion failed" }
    Write-Host "Converting the scene with Blender $blender (a few minutes)..."
    # Blender scrive avvisi su stderr: con "Stop" PowerShell 5.1 li tratterebbe come errori
    $ErrorActionPreference = "Continue"
    & $blender -b --python "$root\tools\zero_day\fbx_to_gltf.py" -- "$work\MEASURE_SEVEN\MEASURE_SEVEN.fbx" "$work\png" "$work\out\MEASURE_SEVEN.gltf" *> "$work\blender.log"
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$work\out\MEASURE_SEVEN.gltf")) {
        Get-Content "$work\blender.log" -Tail 20 | Write-Host
        throw "Blender conversion failed"
    }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item "$work\out\*", "$work\LICENSE.txt", "$work\README.txt" -Destination $dest -Recurse -Force
    # La scena integrata 13 indica le mesh del glTF per indice: un'altra versione di Blender puo'
    # numerarle diversamente. Allora la scena va rigenerata (PT_SAVE_SCENE, vedi tools/zero_day).
    $hash = (Get-FileHash "$dest\MEASURE_SEVEN.gltf" -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne "4cf65c979da11cc4090780cc4b2318505c48ce2acc8e07a306dde9f78a61bffa") {
        Write-Warning "The converted glTF differs from the one scene 13-zero-day was made from (made with Blender 5.2.3); that scene may load wrong meshes."
    }
    Write-Host "Zero-Day installed in $dest"
}

$failed = $false
foreach ($asset in $Assets) {
    if ($all -notcontains $asset) { Write-Host "Unknown asset '$asset'. Known: $($all -join ' ')"; $failed = $true; continue }
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("lucerna-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try { & "Fetch-$asset" $tmp }
    catch { Write-Host "$asset`: $($_.Exception.Message)" -ForegroundColor Red; $failed = $true }
    finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}
if ($failed) { exit 1 }

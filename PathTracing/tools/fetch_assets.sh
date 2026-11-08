#!/bin/bash
# Scarica gli asset grandi che non stanno nel repo (ADR 0022). Si lancia dopo il clone:
#   tools/fetch_assets.sh [asset...]
# Senza argomenti li scarica tutti; un asset gia' presente si salta. Gli asset, e le scene
# integrate che li usano:
#   sponza     Crytek Sponza (jimmiebergmann/Sponza), ~110 MB             -> 08-sponza
#   helmet     DamagedHelmet (Khronos glTF Sample Assets), ~4 MB          -> 09-damaged-helmet
#   spheres    MetalRoughSpheres (Khronos glTF Sample Assets), ~11 MB     -> 10-metal-rough-spheres
#   breakfast  Breakfast Room (McGuire Computer Graphics Archive), ~6 MB  -> 11-breakfast-room
#   bistro     Amazon Lumberyard Bistro (McGuire Archive), ~1.4 GB        -> 12-bistro
#   zeroday    Zero-Day di Beeple (NVIDIA ORCA), ~1.1 GB                  -> 13-zero-day
# Zero-Day arriva in FBX con texture DDS: si converte in glTF con Blender (BLENDER=<eseguibile>,
# altrimenti "blender" dal PATH) e Python con Pillow e numpy (PYTHON=<interprete>), vedi tools/zero_day.
# Ogni download e' fissato a una versione e verificato con SHA-256. Le licenze stanno accanto ai
# modelli e in res/models/CREDITS.md.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
models="$root/res/models"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# download <url> <file> <sha256>
download() {
    curl -fL --progress-bar -o "$2" "$1"
    echo "$3  $2" | sha256sum -c --quiet -
}

fetch_sponza() {
    local dest="$models/Sponza" commit=222338979d32f4f4818466291bdbc29f192b86ba
    [ -f "$dest/sponza.obj" ] && { echo "Sponza is already in $dest"; return; }
    echo "Downloading Sponza (about 110 MB)..."
    curl -fL --progress-bar -o "$tmp/sponza.tar.gz" "https://github.com/jimmiebergmann/Sponza/archive/$commit.tar.gz"
    tar -xzf "$tmp/sponza.tar.gz" -C "$tmp"
    local src="$tmp/Sponza-$commit"
    echo "ec7b65b793692db211e3011d71f8b02f3d3b57ec5c7da90e5805360ce44f452b  $src/sponza.obj" | sha256sum -c --quiet -
    mkdir -p "$dest"
    cp -r "$src/sponza.obj" "$src/sponza.mtl" "$src/textures" "$dest/"
    echo "Sponza installed in $dest"
}

# I modelli Khronos sono file singoli, presi a un commit fisso del repository
khronos=https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models

# fetch_khronos <modello> <sha256 del .bin> <file...>
fetch_khronos() {
    local name=$1 bin_sha=$2; shift 2
    local dest="$models/$name"
    [ -f "$dest/$name.gltf" ] && { echo "$name is already in $dest"; return; }
    echo "Downloading $name..."
    mkdir -p "$tmp/$name"
    for f in "$@" "$name.gltf"; do
        curl -fsSL -o "$tmp/$name/$f" "$khronos/$name/glTF/$f"
    done
    curl -fsSL -o "$tmp/$name/LICENSE.md" "$khronos/$name/LICENSE.md"
    local bin=$(cd "$tmp/$name" && ls *.bin)
    echo "$bin_sha  $tmp/$name/$bin" | sha256sum -c --quiet -
    mkdir -p "$dest"
    cp "$tmp/$name"/* "$dest/"
    echo "$name installed in $dest"
}

fetch_helmet() {
    fetch_khronos DamagedHelmet 61b33ead6aa3de23f39f02aaf0965bc3988575716a768f45b15d5ea3fb783fdf \
        DamagedHelmet.bin Default_AO.jpg Default_albedo.jpg Default_emissive.jpg Default_metalRoughness.jpg Default_normal.jpg
}

fetch_spheres() {
    fetch_khronos MetalRoughSpheres f35a1caf37837b2987e24859d812a23e40ca37e1f741782e2a754961250e8c36 \
        MetalRoughSpheres0.bin Spheres_BaseColor.png Spheres_MetalRough.png
}

mcguire=https://casual-effects.com/g3d/data10/research/model

fetch_breakfast() {
    local dest="$models/BreakfastRoom"
    [ -f "$dest/breakfast_room.obj" ] && { echo "Breakfast Room is already in $dest"; return; }
    echo "Downloading Breakfast Room..."
    download "$mcguire/breakfast_room/breakfast_room.zip" "$tmp/breakfast_room.zip" \
        e56ff9b4c89b680cfd5c4c00390533ae28fff7a122d6fc8c92c273a9066c0ca8
    mkdir -p "$dest"
    unzip -oq "$tmp/breakfast_room.zip" -d "$dest"
    echo "Breakfast Room installed in $dest"
}

fetch_bistro() {
    local dest="$models/Bistro"
    [ -f "$dest/Exterior/exterior.obj" ] && { echo "Bistro is already in $dest"; return; }
    echo "Downloading Bistro (about 1.4 GB)..."
    # Ogni archivio si estrae in una cartella col suo nome: i .mtl puntano a ../BuildingTextures/...
    local parts=(
        "Exterior.zip Exterior f612ba0d8a65811afe0d48a023d31e5b43ac40b78e9b12d15e2b7dc8923c29d4"
        "Interior.zip Interior 3faa41f8d6860d1267268348d44d2b74c64a8bfb55dd4b03c562a70ab83b1062"
        "BuildingTextures BuildingTextures 16538a37f34cadec5665d97e5b77ef3581d67ab674b33b9caeef1d277cbde6bf"
        "OtherTextures OtherTextures 0806fee3eeb0e14cd501582edbd2d5caf33c5416a62a871e0f126391096f5c58"
        "PropTextures PropTextures a179e4ee6367d2b518c4978d7e2e690c59503861634d67d0da8bacfa7196571a"
    )
    for part in "${parts[@]}"; do
        set -- $part
        download "$mcguire/bistro/$1" "$tmp/$1" "$3"
        mkdir -p "$dest/$2"
        unzip -oq "$tmp/$1" -d "$dest/$2"
        rm "$tmp/$1"
    done
    echo "Bistro installed in $dest"
}

fetch_zeroday() {
    local dest="$models/ZeroDay"
    [ -f "$dest/MEASURE_SEVEN.gltf" ] && { echo "Zero-Day is already in $dest"; return; }
    local blender=${BLENDER:-blender} python=${PYTHON:-}
    if ! "$blender" --version > /dev/null 2>&1; then
        echo "Zero-Day skipped: Blender not found (set BLENDER=<path to blender>)" >&2
        return 1
    fi
    if [ -z "$python" ]; then
        for candidate in python3 python; do
            if "$candidate" -c "import PIL, numpy" > /dev/null 2>&1; then python=$candidate; break; fi
        done
    fi
    if [ -z "$python" ] || ! "$python" -c "import PIL, numpy" > /dev/null 2>&1; then
        echo "Zero-Day skipped: needs Python with Pillow and numpy (pip install pillow numpy, or set PYTHON)" >&2
        return 1
    fi
    echo "Downloading Zero-Day (about 1.1 GB)..."
    download https://developer.nvidia.com/beeple "$tmp/ZeroDay_v1.zip" \
        a9d040fe9720ee88937778659205fd76bad4499431a37156aaeeba382f08d592
    # Il "*" di unzip non scende nelle sottocartelle: tex/ va nominata a parte
    unzip -oq "$tmp/ZeroDay_v1.zip" 'MEASURE_SEVEN/*' 'MEASURE_SEVEN/tex/*' LICENSE.txt README.txt -d "$tmp/zeroday"
    rm "$tmp/ZeroDay_v1.zip"
    echo "Converting the textures..."
    "$python" "$root/tools/zero_day/convert_textures.py" "$tmp/zeroday/MEASURE_SEVEN/tex" "$tmp/zeroday/png" > /dev/null
    echo "Converting the scene with Blender (a few minutes)..."
    "$blender" -b --python "$root/tools/zero_day/fbx_to_gltf.py" -- \
        "$tmp/zeroday/MEASURE_SEVEN/MEASURE_SEVEN.fbx" "$tmp/zeroday/png" "$tmp/zeroday/out/MEASURE_SEVEN.gltf" > "$tmp/zeroday/blender.log" 2>&1 \
        || { tail -20 "$tmp/zeroday/blender.log" >&2; return 1; }
    mkdir -p "$dest"
    cp -r "$tmp/zeroday/out"/* "$tmp/zeroday/LICENSE.txt" "$tmp/zeroday/README.txt" "$dest/"
    # La scena integrata 13 indica le mesh del glTF per indice: un'altra versione di Blender puo'
    # numerarle diversamente. Allora la scena va rigenerata (PT_SAVE_SCENE, vedi tools/zero_day).
    if ! echo "4cf65c979da11cc4090780cc4b2318505c48ce2acc8e07a306dde9f78a61bffa  $dest/MEASURE_SEVEN.gltf" | sha256sum -c --quiet - 2> /dev/null; then
        echo "Warning: the converted glTF differs from the one scene 13-zero-day was made from" \
             "(made with Blender 5.2.3); that scene may load wrong meshes." >&2
    fi
    echo "Zero-Day installed in $dest"
}

all=(sponza helmet spheres breakfast bistro zeroday)
if [ $# -eq 0 ]; then assets=("${all[@]}"); else assets=("$@"); fi
failed=0
for asset in "${assets[@]}"; do
    case "$asset" in
        sponza|helmet|spheres|breakfast|bistro|zeroday)
            # Una subshell fuori da ||/if: cosi' set -e vale dentro la funzione (un checksum
            # sbagliato la ferma) e un asset fallito non ferma gli altri
            set +e
            (set -e; "fetch_$asset")
            [ $? -eq 0 ] || failed=1
            set -e ;;
        *) echo "Unknown asset '$asset'. Known: ${all[*]}" >&2; failed=1 ;;
    esac
done
exit $failed

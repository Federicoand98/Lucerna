#!/bin/bash
# Scarica gli asset grandi che non stanno nel repo (ADR 0022). Si lancia una volta dopo il clone:
#   tools/fetch_assets.sh [cartella di destinazione di Sponza]
#
# Sponza: la versione corretta di jimmiebergmann/Sponza (Crytek Sponza di Frank Meinl, CC BY 3.0),
# fissata a un commit e verificata: la scena integrata 08-sponza conta su questi file esatti.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-"$root/res/models/Sponza"}
commit=222338979d32f4f4818466291bdbc29f192b86ba
obj_sha256=ec7b65b793692db211e3011d71f8b02f3d3b57ec5c7da90e5805360ce44f452b

if [ -f "$dest/sponza.obj" ]; then
    echo "Sponza is already in $dest"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "Downloading Sponza (about 110 MB)..."
curl -fL --progress-bar -o "$tmp/sponza.tar.gz" "https://github.com/jimmiebergmann/Sponza/archive/$commit.tar.gz"
tar -xzf "$tmp/sponza.tar.gz" -C "$tmp"
src="$tmp/Sponza-$commit"
echo "$obj_sha256  $src/sponza.obj" | sha256sum -c --quiet -

mkdir -p "$dest"
cp -r "$src/sponza.obj" "$src/sponza.mtl" "$src/textures" "$dest/"
echo "Sponza installed in $dest"

#!/usr/bin/env python3
"""Converte le texture DDS di Zero-Day (NVIDIA ORCA) in PNG/JPEG che Lucerna e Blender leggono.

Uso: convert_textures.py <cartella tex con le .dds> <cartella di uscita>

Servono Pillow e numpy (pip install pillow numpy). Le convenzioni di ORCA (README.txt dell'archivio):
  *_BaseColor  RGB colore, A opacita'  -> PNG se l'alpha e' usata, altrimenti JPEG
  *_Specular   R occlusione, G roughness, B metallic: e' gia' l'ORM del glTF -> JPEG
  *_Normal     BC5 (solo RG), convenzione DirectX -> PNG con B ricostruito e G invertito (OpenGL)
  *_Emissive   RGB emissione -> JPEG
Le texture piu' grandi di MAX_SIZE si dimezzano finche' ci stanno: il renderer le riduce comunque.
Scrive manifest.json: per ogni .dds il file prodotto e se ha un'alpha vera (serve a fbx_to_gltf.py).
"""
import json
import pathlib
import sys

import numpy as np
from PIL import Image

MAX_SIZE = 2048
JPEG_QUALITY = 92


def shrink(im):
    while max(im.size) > MAX_SIZE:
        im = im.resize((max(1, im.width // 2), max(1, im.height // 2)), Image.LANCZOS)
    return im


def normal_to_opengl(im):
    # BC5 porta solo X e Y: Z si ricostruisce dalla lunghezza unitaria; Y si inverte (DirectX -> OpenGL)
    rg = np.asarray(im.convert("RGB"), dtype=np.float32)[..., :2] / 127.5 - 1.0
    x, y = rg[..., 0], -rg[..., 1]
    z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))
    n = np.stack([x, y, z], axis=-1)
    return Image.fromarray(np.clip(np.round((n * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8), "RGB")


def main():
    src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    manifest = {}
    files = sorted(src.glob("*.dds"))
    if not files:
        sys.exit(f"nessuna .dds in {src}")
    for n, path in enumerate(files, 1):
        im = Image.open(path)
        im.load()
        kind = path.stem.rsplit("_", 1)[-1]
        has_alpha = False
        if kind == "Normal":
            im, ext = normal_to_opengl(shrink(im)), "png"
        elif kind == "BaseColor" and im.mode == "RGBA" and im.getextrema()[3][0] < 250:
            im, ext, has_alpha = shrink(im), "png", True
        else:
            im, ext = shrink(im).convert("RGB"), "jpg"
        out = dst / f"{path.stem}.{ext}"
        if ext == "jpg":
            im.save(out, quality=JPEG_QUALITY)
        else:
            im.save(out, optimize=False)
        manifest[path.name] = {"file": out.name, "alpha": has_alpha}
        print(f"[{n}/{len(files)}] {path.name} -> {out.name}", flush=True)
    (dst / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()

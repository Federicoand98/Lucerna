#!/usr/bin/env python3
"""Rigenera una scena integrata dal suo modello, quando cambia un importer (issue #15).

Uso: regenerate_scene.py <scena.json> <modello> [--keep-material NOME ...] [--emission-scale K]
                          [--lucerna ESEGUIBILE]
  <scena.json>  la scena da rigenerare, per esempio res/scenes/builtin/09-damaged-helmet.json
  <modello>     il file importato, relativo a res, per esempio models/DamagedHelmet/DamagedHelmet.gltf

Lucerna reimporta il modello e lo salva (PT_SAVE_SCENE); dalla scena attuale si riportano
inquadratura, sole, sfondo, environment, impostazioni di render e, se le istanze sono le stesse,
le loro trasformazioni (Bistro e' scalato da centimetri a metri). I materiali vengono dall'importer:
--keep-material riporta dalla scena attuale quelli ritoccati a mano, per nome. --emission-scale
moltiplica la strength di tutti gli emettitori: l'intensita' delle luci e' una scelta della scena,
come il sole (Zero-Day, illuminata solo dalle sue texture emissive, vuole 15).
Va lanciato da PathTracing/, dopo il build. Apre una finestra per un frame.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

KEEP = ("camera", "sun", "background", "environmentMapping", "render")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scene")
    parser.add_argument("model")
    parser.add_argument("--keep-material", action="append", default=[])
    parser.add_argument("--emission-scale", type=float, default=1.0)
    default_app = "build/bin/Lucerna.exe" if os.name == "nt" else "build/bin/Lucerna"
    parser.add_argument("--lucerna", default=default_app)
    args = parser.parse_args()

    scene = pathlib.Path(args.scene).resolve()
    old = json.loads(scene.read_text(encoding="utf-8"))
    # Il file nuovo sta accanto al vecchio: i path dei modelli sono relativi alla cartella della scena
    fresh = scene.with_name(scene.stem + ".regenerating.json")
    shot = scene.with_name(scene.stem + ".regenerating.png")
    env = dict(os.environ, PT_GLTF=args.model, PT_SAVE_SCENE=str(fresh), PT_SHOT=str(shot), PT_SHOT_FRAME="1",
               PT_NOVSYNC="1", PT_VIEWPORT="64x64")
    try:
        subprocess.run([str(pathlib.Path(args.lucerna).resolve())], env=env, check=True, stdout=subprocess.DEVNULL)
        new = json.loads(fresh.read_text(encoding="utf-8"))
    finally:
        for f in (fresh, shot):
            f.unlink(missing_ok=True)

    for key in KEEP:
        if key in old:
            new[key] = old[key]
    if len(old["meshes"]) == len(new["meshes"]):
        for o, n in zip(old["meshes"], new["meshes"]):
            n["transform"] = o["transform"]
    else:
        print(f"istanze diverse ({len(old['meshes'])} -> {len(new['meshes'])}): trasformazioni dall'importer",
              file=sys.stderr)

    old_materials = {m["name"]: m for m in old["assets"]["materials"]}
    for name in args.keep_material:
        target = next((m for m in new["assets"]["materials"] if m["name"] == name), None)
        if target is None or name not in old_materials:
            sys.exit(f"materiale '{name}' non trovato")
        kept = dict(old_materials[name])
        for key in ("id", "albedoTexture", "normalTexture", "alphaTexture", "roughnessTexture",
                    "metallicTexture", "emissiveTexture"):
            kept[key] = target.get(key, "")  # gli id sono quelli nuovi
        target.update(kept)

    if args.emission_scale != 1.0:
        for m in new["assets"]["materials"]:
            m["emissiveStrength"] = m["emissiveStrength"] * args.emission_scale

    scene.write_text(json.dumps(new, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{scene.name}: {len(new['assets']['materials'])} materiali, {len(new['meshes'])} istanze")


if __name__ == "__main__":
    main()

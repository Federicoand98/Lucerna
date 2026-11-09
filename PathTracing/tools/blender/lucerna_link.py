# Il collegamento Blender -> Lucerna (ADR 0003, ADR 0024).
#
# Non si installa: Lucerna lancia Blender con
#     blender --python tools/blender/lucerna_link.py -- --lucerna-manifest <file.json>
# e l'addon vive solo in quella sessione. In Blender si apre qualsiasi .blend; la scena si
# esporta in glTF non-binary (.gltf + .bin + texture) in <cartella del .blend>/lucerna_live/
# all'apertura, a ogni salvataggio e su richiesta. Dopo ogni export completo il manifest si
# riscrive con una rename atomica: Lucerna osserva solo quello, quindi non legge mai un glTF a meta'.
#
# Il manifest, versione 1:
#     { "version": 1, "sync": N, "status": "exported" | "unsaved",
#       "blend": "<.blend>", "gltf": "<.gltf>", "exportMs": 123.4 }
# Sync now in Lucerna crea <manifest>.request, che un timer controlla.

import json
import os
import sys
import time

import bpy
from bpy.app.handlers import persistent

REQUEST_POLL_SECONDS = 0.5

_manifest = None  # il path passato da Lucerna dopo "--"
_sync = 0


def _manifest_from_argv():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if "--lucerna-manifest" in argv:
        i = argv.index("--lucerna-manifest")
        if i + 1 < len(argv):
            return argv[i + 1]
    return None


def _write_manifest(status, blend="", gltf="", export_ms=0.0):
    """Scrive il manifest su un file temporaneo e poi lo rinomina: chi legge vede o il vecchio
    o il nuovo, mai uno a meta'."""
    if _manifest is None:
        return
    data = {"version": 1, "sync": _sync, "status": status, "blend": blend, "gltf": gltf, "exportMs": export_ms}
    tmp = _manifest + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, _manifest)


def _export_dir():
    blend = bpy.data.filepath
    return os.path.join(os.path.dirname(blend), "lucerna_live") if blend else None


def sync():
    """Esporta la scena e annuncia l'export a Lucerna. Un file mai salvato non ha dove andare."""
    global _sync
    out_dir = _export_dir()
    if out_dir is None:
        _write_manifest("unsaved")
        print("[Lucerna] save the .blend first: there is nowhere to export it yet")
        return None

    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "scene.gltf")
    start = time.perf_counter()
    # GLTF_SEPARATE: Lucerna carica le texture da file (ADR 0003). Le luci passano con
    # KHR_lights_punctual (ADR 0009); la camera attiva la usa solo il primo sync di un .blend.
    bpy.ops.export_scene.gltf(
        filepath=out_path,
        export_format='GLTF_SEPARATE',
        export_apply=True,
        export_yup=True,
        export_normals=True,
        export_texcoords=True,
        export_materials='EXPORT',
        export_lights=True,
        export_cameras=True,
        use_selection=False,
    )
    export_ms = (time.perf_counter() - start) * 1000.0
    _sync += 1
    _write_manifest("exported", bpy.data.filepath, out_path, export_ms)
    print("[Lucerna] sync %d: %s (%.0f ms)" % (_sync, out_path, export_ms))
    return out_path


@persistent
def _on_save(_):
    # save_post gira dopo il salvataggio; l'export non e' un salvataggio, quindi niente ricorsione
    sync()


@persistent
def _on_load(_):
    # aprire un altro .blend in Blender cambia la scena anche in Lucerna
    sync()


def _check_request():
    """Timer: Sync now da Lucerna e' un file accanto al manifest."""
    request = _manifest + ".request"
    if os.path.exists(request):
        try:
            os.remove(request)
        except OSError:
            pass
        sync()
    return REQUEST_POLL_SECONDS


class LUCERNA_OT_sync(bpy.types.Operator):
    """Export the scene to Lucerna now"""
    bl_idname = "lucerna.sync"
    bl_label = "Sync now"

    def execute(self, context):
        if sync() is None:
            self.report({'WARNING'}, "Save the .blend first")
            return {'CANCELLED'}
        self.report({'INFO'}, "Synced to Lucerna")
        return {'FINISHED'}


class LUCERNA_PT_panel(bpy.types.Panel):
    bl_label = "Lucerna"
    bl_idname = "LUCERNA_PT_panel"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Lucerna"

    def draw(self, context):
        col = self.layout.column()
        col.label(text="Linked to Lucerna" if _manifest else "Not linked: open Blender from Lucerna", icon='LINKED')
        col.operator("lucerna.sync", icon='FILE_REFRESH')
        out = _export_dir()
        col.label(text="Syncs on open and on save" if out else "Save the .blend to start syncing")
        if out:
            col.label(text=out)


def register():
    global _manifest
    _manifest = _manifest_from_argv()
    bpy.utils.register_class(LUCERNA_OT_sync)
    bpy.utils.register_class(LUCERNA_PT_panel)
    if _manifest is None:
        print("[Lucerna] no --lucerna-manifest: the panel works, Lucerna will not see the exports")
        return
    if _on_save not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(_on_save)
    if _on_load not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load)
    bpy.app.timers.register(_check_request, first_interval=REQUEST_POLL_SECONDS, persistent=True)
    _write_manifest("unsaved")  # Blender e' partito: Lucerna aspetta un .blend
    print("[Lucerna] linked, manifest:", _manifest)


if __name__ == "__main__":
    register()

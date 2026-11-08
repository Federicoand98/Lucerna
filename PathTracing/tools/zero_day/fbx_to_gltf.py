# Converte una scena Zero-Day di NVIDIA ORCA (FBX) in glTF per Lucerna. Gira dentro Blender:
#   blender -b --python fbx_to_gltf.py -- <scena.fbx> <texture convertite> <uscita.gltf>
# <texture convertite> e' la cartella scritta da convert_textures.py, con il suo manifest.json.
#
# L'importer FBX di Blender collega le texture di ORCA quasi giuste; qui si sistemano i materiali
# perche' l'esportatore glTF li riconosca:
#   - le immagini puntano ai PNG/JPEG convertiti invece che alle .dds;
#   - *_Specular (R occlusione, G roughness, B metallic) passa da "Specular IOR Level" a
#     Roughness e Metallic tramite Separate Color: l'esportatore ne fa la metallicRoughnessTexture;
#   - l'alpha della base color resta solo dove la texture ne ha una vera, come alpha MASK;
#   - le normal map sono gia' in convenzione OpenGL (convert_textures.py ha invertito il verde);
#   - il Mapping node (ripetizione e offset delle texture) si applica alle UV, perche' Lucerna non
#     legge KHR_texture_transform.
# Geometria: gli oggetti che l'FBX marca invisibili si scartano; le mesh identiche (i cloni di
# Cinema 4D, esportati come copie) tornano istanze di una mesh sola; le altre si uniscono in un
# oggetto, perche' migliaia di oggetti diventerebbero migliaia di BLAS.
import hashlib
import json
import os
import sys
import time

import bpy
import mathutils
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
fbx_path, tex_dir, out_path = (os.path.abspath(a) for a in args[:3])
manifest = json.load(open(os.path.join(tex_dir, "manifest.json"), encoding="utf-8"))

start = time.time()
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_path)
print(f"FBX importato in {time.time() - start:.0f} s")

# ---- immagini: dalle .dds ai file convertiti
for image in bpy.data.images:
    name = os.path.basename(bpy.path.abspath(image.filepath))
    entry = manifest.get(name)
    if entry is None:
        print("immagine senza conversione:", name)
        continue
    image.filepath = os.path.join(tex_dir, entry["file"])
    image.reload()
    if name.endswith(("_Specular.dds", "_Normal.dds")):
        image.colorspace_settings.name = "Non-Color"


def image_entry(node):
    return manifest.get(os.path.basename(bpy.path.abspath(node.image.filepath)).rsplit(".", 1)[0] + ".dds") \
        if node.image else None


# ---- materiali
for material in bpy.data.materials:
    if material.node_tree is None:
        continue
    nodes, links = material.node_tree.nodes, material.node_tree.links
    bsdf = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        continue
    for link in list(bsdf.inputs["Specular IOR Level"].links):
        texture = link.from_node
        links.remove(link)
        separate = nodes.new("ShaderNodeSeparateColor")
        links.new(texture.outputs["Color"], separate.inputs["Color"])
        links.new(separate.outputs["Green"], bsdf.inputs["Roughness"])
        links.new(separate.outputs["Blue"], bsdf.inputs["Metallic"])
    bsdf.inputs["Specular IOR Level"].default_value = 0.5
    for link in list(bsdf.inputs["Alpha"].links):
        texture = link.from_node
        links.remove(link)
        entry = image_entry(texture) if texture.type == "TEX_IMAGE" else None
        if entry and entry["alpha"]:
            # "Greater Than" tra la texture e l'Alpha e' il modo in cui l'esportatore riconosce il MASK
            cutoff = nodes.new("ShaderNodeMath")
            cutoff.operation = "GREATER_THAN"
            cutoff.inputs[1].default_value = 0.5
            links.new(texture.outputs["Alpha"], cutoff.inputs[0])
            links.new(cutoff.outputs[0], bsdf.inputs["Alpha"])

# ---- Mapping node -> UV. Un materiale ha un solo Mapping per tutte le sue texture (lo crea
# l'importer FBX); dopo averlo applicato alle UV lo si riporta all'identita'.
def mapping_matrix(material):
    if not material or material.node_tree is None:
        return None
    mappings = [n for n in material.node_tree.nodes if n.type == "MAPPING"]
    if not mappings:
        return None
    def matrix_of(node):
        location = node.inputs["Location"].default_value
        rotation = node.inputs["Rotation"].default_value
        scale = node.inputs["Scale"].default_value
        m = mathutils.Matrix.LocRotScale(location.copy(), mathutils.Euler(rotation.copy()), scale.copy())
        if node.vector_type == "TEXTURE":
            return m.inverted()
        if node.vector_type != "POINT":
            print("Mapping di tipo", node.vector_type, "non gestito:", material.name)
        return m

    # L'importer FBX mette un Mapping per texture: le UV sono una sola, quindi devono coincidere
    matrices = [matrix_of(n) for n in mappings]
    matrix = matrices[0]
    if any(not np.allclose(np.array(m), np.array(matrix), atol=1e-5) for m in matrices[1:]):
        print("Mapping diversi fra le texture, si usa il primo:", material.name)
    for node in mappings:
        node.inputs["Location"].default_value = (0, 0, 0)
        node.inputs["Rotation"].default_value = (0, 0, 0)
        node.inputs["Scale"].default_value = (1, 1, 1)
    if matrix == mathutils.Matrix.Identity(4):
        return None
    return np.array(matrix, dtype=np.float64)


mappings = {m.name: mapping_matrix(m) for m in bpy.data.materials}

# ---- geometria
# Oggetti invisibili nell'FBX: via
hidden = [o for o in bpy.data.objects if o.type == "MESH" and not o.visible_get()]
for o in hidden:
    bpy.data.objects.remove(o)
print(f"scartati {len(hidden)} oggetti invisibili")

meshes = [o for o in bpy.data.objects if o.type == "MESH"]
for o in meshes:
    mesh = o.data
    if not mesh.uv_layers or not mesh.polygons:
        continue
    uv = np.empty(len(mesh.loops) * 2)
    mesh.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    poly_material = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("material_index", poly_material)
    loop_total = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", loop_total)
    loop_material = np.repeat(poly_material, loop_total)
    changed = False
    for slot_index, slot in enumerate(o.material_slots):
        matrix = mappings.get(slot.material.name) if slot.material else None
        if matrix is None:
            continue
        rows = loop_material == slot_index
        if rows.any():
            uv[rows] = uv[rows] @ matrix[:2, :2].T + matrix[:2, 3]
            changed = True
    if changed:
        mesh.uv_layers.active.data.foreach_set("uv", uv.ravel())


# Mesh identiche -> istanze di una sola. La chiave e' geometria in spazio locale, UV e materiali.
def geometry_key(o):
    mesh = o.data
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", co)
    corners = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corners)
    digest = hashlib.sha1(np.round(co, 4).tobytes())
    digest.update(corners.tobytes())
    if mesh.uv_layers:
        uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
        mesh.uv_layers.active.data.foreach_get("uv", uv)
        digest.update(np.round(uv, 4).tobytes())
    digest.update("|".join(s.material.name if s.material else "" for s in o.material_slots).encode())
    return digest.hexdigest()


canonical = {}
for o in meshes:
    key = geometry_key(o)
    if key in canonical:
        old = o.data
        o.data = canonical[key]
        if old.users == 0:
            bpy.data.meshes.remove(old)
    else:
        canonical[key] = o.data
instanced = sum(1 for o in meshes if o.data.users > 1)
print(f"{len(meshes)} oggetti, {len(canonical)} mesh diverse; {instanced} oggetti sono istanze")

# Le mesh usate una volta sola in un unico oggetto
bpy.ops.object.select_all(action="DESELECT")
unique = [o for o in meshes if o.data.users == 1]
for o in unique:
    o.select_set(True)
if len(unique) > 1:
    bpy.context.view_layer.objects.active = unique[0]
    bpy.ops.object.join()
mesh_objects = [o for o in bpy.data.objects if o.type == "MESH"]
print(f"unite {len(unique)} mesh; oggetti mesh: {len(mesh_objects)}, triangoli unici:",
      sum(len(m.polygons) for m in bpy.data.meshes if m.users))

camera = next((o for o in bpy.data.objects if o.type == "CAMERA"), None)
if camera:
    bpy.context.scene.camera = camera

os.makedirs(os.path.dirname(out_path), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=out_path, export_format="GLTF_SEPARATE", export_texture_dir="textures",
                          export_image_format="AUTO", export_cameras=True, export_lights=False,
                          export_yup=True, export_apply=False)
print(f"glTF scritto in {time.time() - start:.0f} s: {out_path}")

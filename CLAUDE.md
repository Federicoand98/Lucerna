# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

The file imported above covers build commands, presets, runtime paths, entry points and dependencies. This file adds what it doesn't.

## Build and Run Notes

- `PathTracing/run.sh` builds, then runs the app from `res/`.
- GLFW 3.5.1 is built from the `deps/glfw` submodule (`git submodule update --init`), with Wayland and X11 backends, after CMake copies it into `build/glfw-patched` and applies `tools/glfw/interactive-move-resize.patch` (upstream PR #2834's `glfwStartInteractiveMove` plus our `glfwStartInteractiveResize`). The submodule itself stays untouched; a changed patch is reapplied on the next configure. Drop the patch when GLFW ships the API (ADR 0021).
- Sources are found with `GLOB_RECURSE`, so re-run the CMake configure step after adding a `.cpp` or `.h` file.
- The project layout (ADR 0022): `src/` is C++, `shaders/` the GLSL sources, `res/` only the data read at runtime (fonts, textures, models, `scenes/builtin`), and everything generated goes to `build/`: the executable in `build/bin/`, the SPIR-V in `build/shaders/`. The app finds `res` and the SPIR-V through paths CMake bakes in (`LUCERNA_RESOURCE_DIR`, `LUCERNA_SHADER_DIR`; the env vars `LUCERNA_RESOURCES` and `LUCERNA_SHADERS` override them) and chdirs into `res` at startup, so data paths stay relative to it. The user's state lives in `~/.config/lucerna` (`imgui.ini`, `preferences.json`) and screenshots in the Pictures folder (`Core/Paths.h`).
- After cloning, run `tools/fetch_assets.sh [asset...]` (`.ps1` on Windows) once: it downloads the models the built-in scenes 08-13 need and the repo doesn't hold (Sponza, DamagedHelmet, MetalRoughSpheres, Breakfast Room, Bistro, Zero-Day), each pinned and checked by SHA-256; credits and licenses are in `res/models/CREDITS.md`. Zero-Day comes from NVIDIA ORCA as FBX with DDS textures and is converted to glTF by `tools/zero_day/` (Python with Pillow and numpy, then Blender headless); built-in scene 13 refers to its meshes by index, so a conversion with another Blender version may need the scene regenerated.
- Shaders compile to SPIR-V at build time with `glslc`, from `shaders/` into `build/shaders/`. After editing one, `cmake --build build --target Shaders` is enough. `glslc` resolves `#include` itself and depends on every `.glsl`.
- To test other shaders without rebuilding, point `LUCERNA_SHADERS` at another folder of `.spv` files.
- Rendering has no automated tests: verify it with the screenshot mode below. Logic that runs without a GPU has `tests/Checks.cpp`, built as `build/LucernaChecks`; run it after touching `Core` or `Scene`, and add a check when a change has edge cases a render can't show.

## Scripted Runs

`PT_*` environment variables drive the app for repeatable captures. It still opens a GLFW window, so it needs a display and a GPU. Run from any folder; file paths in `PT_*` variables are relative to where you launch it (an input not found there falls back to `res`, so `PT_GLTF=models/...` works), and a run with `PT_SHOT` neither reads nor writes the window layout:

```bash
PT_NOVSYNC=1 PT_VIEWPORT=1280x720 PT_SCENE=6 PT_SHOT=shot.png PT_SHOT_FRAME=64 ./build/bin/Lucerna
```

| Variable | Effect |
|---|---|
| `PT_SCENE=<n or name>` | Startup built-in scene, by the number prefix or the name of its file in `res/scenes/builtin` (`6` or `setup-2`) |
| `PT_GLTF=<file>` | Open a file instead, in any format the importer registry knows (OBJ, glTF) |
| `PT_SCENE_FILE=<file.json>` | Open a saved scene (format 2 or 3) instead |
| `PT_SAVE_SCENE=<file.json>` | Save what the variables above opened and set as a scene file, before the window opens: with `PT_GLTF` and `PT_CAM` it turns a model into a built-in scene, whose sun and settings are then edited in the JSON |
| `PT_SHOT=<file>`, `PT_SHOT_FRAME=<n>` | Save a screenshot after n frames (default 200), then exit |
| `PT_HDR=<file>` | With `PT_SHOT`, also dump linear beauty: int width, int height, then RGBA float32 |
| `PT_VIEWPORT=<W>x<H>` | Fix the render size; without it the ImGui layout changes resolution between runs |
| `PT_NOVSYNC=1` | Disable vsync |
| `PT_PLATFORM=x11\|wayland` | Force GLFW's platform; the default is native Wayland when available |
| `PT_NEE=0\|1`, `PT_DEPTH=<n>`, `PT_REPROJ=0\|1`, `PT_DENOISE=0\|1` | Force NEE, ray depth, temporal reprojection, or the denoiser |
| `PT_CAM=x,y,z,dx,dy,dz` | Camera position and view direction, for repeatable framings (Sponza's default camera sits in the dark arcade) |
| `PT_AOV=<n>` | Start in a debug view: 1 albedo, 2 normal, 3 depth |
| `PT_VALIDATION=1` | Enable the Vulkan validation layers in a Release build; errors print as `ERROR::VULKAN` |
| `PT_LOG_FPS`, `PT_LIGHTS` | Log FPS with GPU pass times, or emitter counts, to stdout |

To see the whole UI rather than the render, run the app with `PT_PLATFORM=x11` so it runs under XWayland (Wayland has no way to capture another app's window from a script), find the window with `xwininfo -root -tree | grep '("Lucerna" "Lucerna")'`, and capture it with `import -window <id> out.png`.

Compare renders in linear space (`PT_HDR`), not on tonemapped PNGs: tonemapping and 8-bit clipping both darken the noisier image and skew the result (ADR 0004).

With a fixed viewport, runs are deterministic, so a refactor must give an exact pixel match. Measure GPU times with `PT_NOVSYNC=1`, and treat results from an occluded window as invalid: the compositor throttles it.

## Modules

`src/` has one folder per module, and each module is a static library declared in `CMakeLists.txt` with `lucerna_module(<Name> DEPS ...)`. Includes are written from the `src` root (`#include "Render/Renderer.h"`), everything is in namespace `Lucerna`, and file names match their class.

| Module | Holds | May include |
|---|---|---|
| `Core` | Random, Ray, ObjectId, `SceneObject`/`ObjectList`, the field-table serializer (`Fields.h`, `Json.h`) | nothing |
| `Assets` | Material, Texture, MeshAsset and Triangle, `AssetLibrary` | Core |
| `Scene` | `Scene` (objects, lights, sun, shot, environment, edit API), the primitives and the `Mesh` instance | Core, Assets |
| `Import` | the importers (`ObjImporter`, `GltfImporter`), their registry (`Importer.h`), `OpenFile`/`AddFile`/`LoadMissingGeometry` (`SceneFiles.h`), the OBJ/MTL parser | Core, Assets, Scene |
| `Vulkan` | context, compute pipelines, texture arrays | Core |
| `Render` | Renderer, RenderScene and the GPU structs (`GpuTypes.h`), PathTracer, Denoiser, SceneAccel, RenderSettings, RenderView | Core, Assets, Scene, Vulkan |
| `Serialization` | SceneSerializer, the built-in scene list (`BuiltinScenes.h`) | Core, Assets, Scene, Import, Render |
| `Editor` | Application, the workspace panels (`Workspace.cpp`), theme, widgets and icons, Document, the navigating Camera, Input, Preferences | everything, plus ImGui |

`tools/check_modules.py` runs as part of the build and fails it when a file includes a module outside its row, uses ImGui outside `Editor`, or sits at the top of `src` (only `Main.cpp` and `ptpch.h` may). A new module needs a `lucerna_module` line; a new dependency needs a `DEPS` entry, which is the point where to ask whether it belongs. The renderer never sees the navigating camera: the Editor hands it a `RenderView` (matrices and position) every frame, and registers the renderer's display image with ImGui itself.

Scene objects and assets live in `ObjectList<T>` (ADR 0012): each element is a `SceneObject<T>`, the data plus a 64-bit `ObjectId` and a name unique within the list. Add them with `Add` (never construct them by hand) and refer to them by id outside a single frame. Assets (ADR 0015) are in the `AssetLibrary`, which lives next to the `Scene`: mesh geometry (`MeshAsset`, keyed by its `MeshSource`: an OBJ file, or a glTF mesh and primitive), materials and textures. Scene objects reference them by id: a `Mesh` is an instance (transform, asset id, material for the triangles without a slot), so the same model placed twice is loaded once and has one BLAS. A triangle's `MaterialSlot` indexes its asset's `MaterialSlots`. Nothing in the scene or the assets is a GPU struct: `RenderScene` turns ids into buffer indices and texture layers.

## Scene Architecture

The scene is four layers, each in its own place (ADR 0018). The `Application` owns a `Scene` and an `AssetLibrary` and hands both to whoever needs them.

| Layer | Type | Holds | Never contains |
|---|---|---|---|
| Description | `Scene` (`src/Scene`) | spheres, quads, boxes, mesh instances, lights, sun, shot, environment | assets, GPU structs, file formats |
| Assets | `AssetLibrary` (`src/Assets`) | mesh geometry, materials, textures, each with an id | scene objects, GPU structs, file formats |
| GPU representation | `RenderScene` (`src/Render`) | buffers, light list, BLAS/TLAS, texture arrays, the cubemap | anything editable: it is rebuilt from the two above |
| Files | importers (`src/Import`), `SceneSerializer` | reading formats into the two editable layers, saving scene files | state |

Where new things go:
- **A new object type** (say, a disk): a scene struct with a `JsonFields` table in `Scene/Primitives.h`, an `ObjectList` plus `Set`/`Add`/`Remove` and a change flag in `Scene`, its GPU struct in `Render/GpuTypes.h` and its upload in `RenderScene::Sync`, a line in `SceneSerializer`, and the shader side.
- **A new file format**: an importer in `src/Import` and one line in `Importers()`.
- **A new built-in scene**: a scene file saved as `res/scenes/builtin/NN-name.json`.
- **A new kind of asset**: an `ObjectList` with its edit methods and flag in `AssetLibrary`, and what `RenderScene` makes of it.

## Architecture

The compute shader is the renderer, on Vulkan 1.3 with `VK_KHR_ray_query` (ADR 0005). `shaders/PathTracing.comp` holds `main` and includes the rest from `pt/` and `common/`, in dependency order. The C++ side owns the scene, and `RenderScene` (ADR 0013) turns it into everything the GPU consumes: buffers, light list, acceleration structures, texture arrays and the environment cubemap. The path tracer reads only the `RenderScene`, never the `Scene` or the assets; the scene's `Mesh` has no GPU-only fields, and the GPU `MeshInfo` with inverse, world AABB and `NumOpaque` is derived from it. `Renderer::Render()` records one frame on the frame command buffer: path trace, à-trous denoise, then tonemap to an RGBA8 image shown in the ImGui viewport.

**Vulkan conventions.** `VulkanContext` keeps one frame in flight, so after `BeginFrame` nothing is in use by the GPU; only what this frame already references goes through `Defer`. Compute images stay in `GENERAL` and synchronise with memory barriers. Descriptors are pushed per dispatch (`ComputePipeline`), which is what makes ping-pong trivial: swapping two images means binding them differently. Uploads run through `Immediate`, outside the frame command buffer.

**Change tracking.** The collections of the `Scene` and of the `AssetLibrary` are private: read them through the const accessors (`Spheres()`, `Materials()`...) and change them by copying an object, editing the copy and handing it back with `Set(id, copy)`, or with the `Add*`/`Remove*`/`SetSun`/`SetCamera`/`SetBackground` calls (materials and textures through the `AssetLibrary`: `Set`, `AddMaterial`, `RegisterTexture`, `RemoveTexture`). Each records a `SceneChange` flag (`Core/SceneChange.h`, scene flags and asset flags); `Renderer::Render` takes both sets once per frame, `RenderScene::Sync` updates only those parts (a moved mesh rebuilds the TLAS, a material re-uploads materials and the light list, triangles and BLASes only on `GeometryChanged`), and any change restarts accumulation. Render settings restart it by themselves too: the `Renderer` compares the integrator fields with last frame (`AccumulationKey` in `Renderer.cpp`), so the UI never resets by hand. `Rename` and `MoveCamera` (the navigating view carrying the shot) record nothing. A new edit goes through a `Scene` or `AssetLibrary` method that sets the right flag, and a new setting that changes the samples goes into `AccumulationKey` (ADR 0014).

**Accumulation stores irradiance, not beauty.** Colour is demodulated by a clamped albedo, integrated in ping-pong textures (`m_Irradiance[2]`, `m_NormalDepth[2]`), and remodulated for display, so anything reading those buffers must remodulate with the same clamped albedo. Moving-camera history is reprojected and capped by `HistoryCap`; a still camera converges without limit (ADR 0002).

**The denoiser is a read-only view filter.** It never writes back into accumulation, and turning it off shows the reference image. Temporal feedback belongs to the accumulator, not the denoiser (ADR 0001).

**Render targets change size at runtime.** With `RenderScale < 1` the app renders smaller while the camera moves, and picking forces full resolution. Size shader work from `width` and `height`, not from the viewport.

**Ray query.** `SceneAccel` builds one BLAS per mesh in local space, a procedural AABB BLAS for spheres and quads, and a TLAS with one instance per mesh plus one for the primitives. `TraceRay` and `Occluded` in `pt/scene.glsl` run the query loop: opaque triangles commit by themselves, AABB candidates are intersected in the shader by `IntersectProcedural`, and masked triangle candidates are confirmed by `AlphaPasses`. A mesh BLAS has up to two geometries, opaque triangles then masked ones: `RenderScene` partitions each mesh's triangles on the GPU copy (the asset keeps load order) and stores the split in `MeshInfo.NumOpaque`, so a triangle's index comes from `TriangleIndex`, never from `FirstTriangle + primitive` (ADR 0008). Primitive indices count spheres first, then quads (`PrimitiveBounds` in `RenderScene.cpp`). Meshes are hit from both sides, and only refractive materials treat a back face as `Inside`. Vulkan has no empty buffers, so counts come from `FrameParams`, never `.length()`.

**Direct lighting** uses NEE with MIS on emissive spheres and quads, on the Sun, and on point and spot lights, all in one light list with uniform selection. Point and spot lights are delta: `SampleLight` reports them with `delta`, their MIS weight is 1, and no BSDF ray can hit them. glTF light intensities are divided by 683 into Blender's radiometric units (ADR 0009). The Sun is specified in irradiance and is also passed in `FrameParams`, so a BSDF ray that escapes into its disc sees it even with NEE off (ADR 0006). Emissive mesh triangles and the environment are not in the light list. With a normal map, `HitInfo.Normal` is the shading normal and `HitInfo.GeoNormal` the one without the map: ray offsets and the horizon test use `GeoNormal`, everything else uses `Normal` (ADR 0007). Sampling and evaluation share `GetBsdfParams` and `CombinedPdf`, so lobe selection must not diverge between them (ADR 0004).

**Scene sources:** built-in scenes are scene files in `res/scenes/builtin`, one per scene, named `NN-name.json` (ADR 0017): adding one means saving a scene there, and the combo and `PT_SCENE` list them through `ListBuiltinScenes`. Each brings its shot, environment and render settings. Other sources are files through the importer registry (ADR 0016), and JSON in `res/scenes/` through `SceneSerializer` (format 3: an `assets` block, objects referencing ids, mesh assets saved as their source and reloaded by `LoadMissingGeometry`, and asset paths written relative to the scene file's folder so a scene moves with its models, `Core/Paths.h`; format 2, with paths relative to `res/`, still loads; older files are refused). The Blender live-link is one-way: an addon (`tools/blender/`) exports glTF on save, and `PollLiveLink` reopens the opened file, any format, when its mtime changes (ADR 0003).

**Document.** The open scene is a `Document` (`src/Editor/Document.h`, ADR 0019): it knows which file it came from (a scene file, an imported file, a built-in scene, or a new scene) and whether there are unsaved changes, by comparing `Scene::Revision()`, `AssetLibrary::Revision()` and the `RenderSettings` with their values at the last save. Every edit bumps the revision, renames included; `MoveCamera` (navigation) does not. Save rewrites only a scene file; anything else goes through Save As. Actions that would discard changes (New, Open, a built-in scene, closing the window, a live-link reload) go through `Application::WhenSaved`, which asks first; scripted runs (`PT_SHOT`) never ask and never touch the recent files. The File menu (`DrawMainMenu`) holds New, Open, Open Recent, Built-in Scenes, Save, Save As and Quit.

**Importers.** Each file format is an `Importer` in `src/Import/`: it reads a file into the `AssetLibrary` (which dedups mesh sources and textures) and into an `ImportedScene` (instances, lights, optional sun, camera and background), and it reloads the geometry of its assets for a scene file. It never sees the `Scene`. `OpenFile` (`Import/SceneFiles.h`) replaces scene and assets with a file (on failure both stay as they were; a file without a camera keeps the current shot), `AddFile` appends a file's meshes and lights at a placement with a fallback material. A new format is a new importer plus one line in `Importers()` (`Importer.cpp`); the file dialogs and `FindImporter` pick it up from there.

## Changing GPU Data

Textures are assets with a kind: color textures go into the sRGB albedo array, data textures (normal maps, MTL alpha masks) into their own UNORM `TextureArray` (binding 19); `RenderScene` assigns the layers. The GPU structs are in `src/Render/GpuTypes.h` and only `RenderScene` fills them. Adding a `Material` field touches `src/Assets/Material.h` (the struct and its `JsonFields` table), `GpuMaterial` in `GpuTypes.h`, `ToGpu` in `RenderScene.cpp`, the struct in `shaders/pt/types.glsl`, and usually the importers (`GltfImporter.cpp`, `ObjImporter.cpp`) and the ImGui panel; a field missing from `ToGpu` silently keeps its default on the GPU. GPU layouts follow std430 with explicit padding, enforced by `static_assert` on size and offsets; keep those in step with the shader. Spheres and quads follow the same pattern (`GpuSphere`, `GpuQuad`). Bindings are in `pt/bindings.glsl`, and `FrameParams` there mirrors the C++ struct in `PathTracer.h`. `DEVELOPER_GUIDE.md` still describes the OpenGL version.

## Settings and Serialization

Settings are split by what they answer (ADR 0010): `Renderer::Settings` (`RenderSettings`: how the image is computed, saved in the scene's `render` block, with Draft/Preview/Final presets), `Renderer::View` (`ViewSettings`: debug view and accumulation pause, never saved), `Preferences` (Vsync, in `~/.config/lucerna/preferences.json`), and `Scene::EnvironmentMapping`. The shot (position, direction, FOV, aperture, focus) is `Scene::Camera`, a `SceneCamera` saved with the scene; the viewport's `Camera` navigates and borrows its lens. With `View.LockViewToCamera` navigation moves the shot, and whenever the shot changes from outside the view follows it (`Application::SyncCameras`, ADR 0011). Scenes set their own shot in their scene file, not in `Application`. A new setting goes in the struct that matches its nature, not on `Renderer`.

Any struct becomes serializable by declaring a `JsonFields` table next to it (`src/Core/Fields.h`): pairs of explicit JSON key and member pointer, or `Custom` with a read and a write function. Including `src/Core/Json.h` then gives `json j = value;` and `j.get_to(value)`, also for nested structs, vectors and glm types. A missing key keeps the struct's default and an unknown key is ignored, so adding a field never breaks saved files. A field left out of the table is not saved.

## UI

The editor follows concept A of the redesign (ADR 0020): a menu bar (File, Edit, Add, View, Help), the Outliner and Materials panels on the left, the Viewport with its toolbar and readouts in the middle, Properties on the right with an icon rail choosing the section (Object, Material, World, Camera, then Render, Film, and Developer when View > Developer is on), and a status bar. The panels are ImGui docking windows; `BuildDefaultLayout` arranges them the first time (an `imgui.ini` that doesn't know "Outliner") and from View > Reset layout, and the window names are the `imgui.ini` keys.

- **Look**: every color, rounding and spacing is in `Editor/Theme.cpp`; named colors (`Theme::Accent`, `Theme::TextDim`...) are for custom drawing. Fonts are Inter, Inter SemiBold and JetBrains Mono in `res/fonts`, loaded at the monitor's content scale.
- **Fields**: property rows use the `UI::` wrappers in `Editor/Widgets.h` (label on the left, field to the edge, numbers in mono, amber slider fill). They take ImGui's signatures; use them instead of `ImGui::SliderFloat` and friends in the properties panel. `UI::Section` is a collapsible section, `UI::Pill` a toolbar button.
- **Window**: on Wayland, X11 and Windows the window has no system frame: the menu bar is the title bar (drag to move, double-click to maximize, the minimize/maximize/close buttons on the right) and a 5 px band along the edges resizes it, all through `glfwStartInteractiveMove`/`Resize`, so the compositor does the work (`DrawWindowControls`, `HandleWindowEdges`). Edit > Preferences > System title bar brings the frame back on the next start. On Wayland ImGui's multi-viewport is off (windows can't be positioned there), and coordinates are logical: `m_UiScale` scales layout, fonts are rasterized at the content scale, and the render and picking use framebuffer pixels (`m_FramebufferScale`). A content-scale change (another monitor) rebuilds fonts and the ImGui Vulkan backend. On Wayland the app draws its own cursor and GLFW only reads `XCURSOR_THEME`/`XCURSOR_SIZE`, which GNOME doesn't set: `UseDesktopCursorTheme` fills them from gsettings before `glfwInit` (the startup `Window:` log line shows the theme in use).
- **Icons**: Lucide glyphs merged into the text fonts, written inside strings (`ICON_SUN " Sun"`). A new icon is a line in `Editor/Icons.h` with its codepoint from lucide-static's `font/info.json`.

## Project Language

`CONTEXT.md` is the glossary. Use its terms (Accumulation, History, Disocclusion, Emitter, AOV, and so on) and avoid the synonyms it lists. Decisions live in `docs/adr/`; check them before changing the integrator, denoiser, lighting, or interop. Code comments and docs are in Italian, while commit messages are in English (`feat:`, `fix:`).

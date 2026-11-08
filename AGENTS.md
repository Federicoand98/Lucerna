# AGENTS.md

## Project Root That Matters
- The git repo root is not the CMake project root. Build and run from `PathTracing/`.
- Main build files live in `PathTracing/CMakeLists.txt`, `PathTracing/CMakePresets.json`, and `PathTracing/build.sh`.

## Verified Build Commands
- Linux/default local build: `cmake -S PathTracing -B PathTracing/build && cmake --build PathTracing/build`
- Repo-provided helper does the same thing from `PathTracing/`: `./build.sh`
- Windows (Visual Studio 2022 + Vulkan SDK): `.\build.ps1` / `.\run.ps1` from `PathTracing/`, which pick the CMake bundled with Visual Studio (an older one on PATH may not know VS 2022). The generator is multi-config, but outputs land in the same places as on Linux (`build/bin/Lucerna.exe`, `build/LucernaChecks.exe`): Debug and Release overwrite each other. `tools/fetch_assets.ps1` and `tools/render_check/capture.ps1` mirror the bash scripts, which also run from Git Bash (`.gitattributes` keeps `*.sh` at LF).
- CI configures the nested project directly: `cmake -B $GITHUB_WORKSPACE/PathTracing/build -DCMAKE_BUILD_TYPE=Release` then `cmake --build ...`
- The only test target is `LucernaChecks` (`tests/Checks.cpp`), CPU-only checks of Core and Scene logic: run `./build/LucernaChecks` from `PathTracing/`. There is no lint or typecheck target; rendering is verified with screenshots (see CLAUDE.md).

## Presets
- `PathTracing/CMakePresets.json` is mostly Windows-oriented (`x64-debug`, `x64-release`, `release`).
- The only Linux preset is literally named `GCC 13.2.0 x86_64-linux-gnu`; use quotes if invoking it.

## Run/Runtime Gotchas
- The project is called Lucerna: the CMake project, the executable and the C++ namespace are `Lucerna`, while the repo folders and the `PT_*` variables keep their old names.
- The executable is emitted to `PathTracing/build/bin/Lucerna` and the SPIR-V to `PathTracing/build/shaders/`; GLSL sources are in `PathTracing/shaders/`. Run it from any folder: it finds `PathTracing/res` through a path CMake bakes in (override with `LUCERNA_RESOURCES`).
- Asset paths are relative to `res` (`models/bunny.obj`, `textures/...`): `Main.cpp` changes directory into it. Window layout and preferences live in `~/.config/lucerna`. The models of built-in scenes 08-13 (Sponza, DamagedHelmet, MetalRoughSpheres, Breakfast Room, Bistro, Zero-Day) are not in the repo: `PathTracing/tools/fetch_assets.sh` downloads them.
- The app requires Vulkan 1.3 and a GPU with `VK_KHR_ray_query`, `VK_KHR_acceleration_structure` and `VK_KHR_push_descriptor`. Building needs `glslc`.

## Real Entry Points
- `PathTracing/src/Main.cpp`: creates `PathTracer::Application`, initializes a `1600x900` window, then enters the main loop.
- `PathTracing/src/Vulkan/VulkanContext.cpp`: instance, device, swapchain, frame lifecycle, buffer and image creation.
- `PathTracing/src/Editor/Application.cpp`: owns GLFW/ImGui setup, the frame loop, viewport sizing, and all runtime UI controls that reset accumulation.
- `PathTracing/src/Render/Renderer.cpp`: records the path tracer, denoiser and tonemap compute passes.
- `PathTracing/src/Render/RenderScene.cpp`: builds the GPU representation of the scene (buffers, light list, BLAS/TLAS, textures) from the `Scene` and the `AssetLibrary`, updating only what changed.
- `PathTracing/src/Render/PathTracer.cpp`: fills the frame uniform block and binds the `RenderScene` for the dispatch.
- `PathTracing/src/Render/SceneAccel.cpp`: BLAS and TLAS construction.
- `PathTracing/src/Scene/Scene.cpp` and `PathTracing/src/Assets/AssetLibrary.cpp`: the scene description and the assets, with their edit APIs. Files come in through `OpenFile`/`AddFile` (`PathTracing/src/Import/SceneFiles.h`) and the importers. Built-in scenes are JSON files in `PathTracing/res/scenes/builtin`. See "Scene Architecture" in CLAUDE.md for where new object types, formats and scenes go.

## Change Hotspots
- If you change scene/material/primitive data layout, update both CPU structs and the compute shader bindings/layout together. Bindings are declared in `PathTracing/shaders/pt/bindings.glsl` and pushed in `PathTracer::Dispatch`.
- Scene edits are live-driven from ImGui through the `Scene` and `AssetLibrary` edit APIs (`Set(id, copy)`, `Add*`, `Remove*`), which record what changed; the renderer syncs only that and restarts accumulation by itself. A new runtime-editable parameter needs a `Scene` or `AssetLibrary` method that sets the right `SceneChange` flag, or, for a render setting that changes the samples, an entry in `AccumulationKey` (`Renderer.cpp`).
- `Renderer::OnResize()` recreates the render targets when the viewport changes; viewport-related bugs usually involve `Application::Render()` plus this method.

## Dependencies And Vendor Code
- GLFW 3.5.1 is built from the git submodule `PathTracing/deps/glfw` (run `git submodule update --init`) with a local patch from `PathTracing/tools/glfw/`, applied by CMake to a copy in the build directory; it needs the Wayland, xkbcommon and X11 development packages. Vulkan comes from `find_package(Vulkan REQUIRED)`.
- GLM is vendored under `PathTracing/src/vendor/glm` and included directly.
- All third-party code lives under `PathTracing/src/vendor`. Dear ImGui (`vendor/imgui`), ImGuizmo, stb and cgltf are built as their own static libraries; nlohmann/json and portable-file-dialogs are header-only.

## Existing Docs
- `DEVELOPER_GUIDE.md` has useful architecture pointers but contains stale claims; prefer `CMakeLists.txt`, CI, and source code when they disagree.

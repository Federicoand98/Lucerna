cmake_minimum_required (VERSION 3.8)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()


project ("Lucerna")

# Senza un build type esplicito CMake non passa NESSUN flag di ottimizzazione (-O0): tutto
# il lato CPU (parsing OBJ, costruzione BVH/SBVH) gira non ottimizzato e molto lento. Default
# a Release quando non specificato; per debuggare basta -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()


set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res")               # i dati letti a runtime
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")       # i sorgenti GLSL
set(SPV_DIR "${CMAKE_BINARY_DIR}/shaders")                  # gli SPIR-V compilati: output del build

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# L'eseguibile e' output del build: sta in build/bin, non in res. Trova i suoi dati dai path
# che CMake gli scrive dentro (LUCERNA_RESOURCES e LUCERNA_SHADERS li sovrascrivono a runtime).
# "$<0:>" e' un'espressione vuota: basta a impedire ai generatori multi-config (Visual Studio)
# di aggiungere la sottocartella Release/Debug, cosi' i path sono gli stessi su ogni piattaforma.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<0:>)
add_compile_definitions(LUCERNA_RESOURCE_DIR="${RES_DIR}" LUCERNA_SHADER_DIR="${SPV_DIR}")

# Windows: windows.h (incluso da portable-file-dialogs e GLFW) definisce le macro min/max, che
# rompono std::min/glm::min. MSVC vuole anche /utf-8 (le icone sono stringhe UTF-8), /bigobj per
# i file grandi e un build parallelo.
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  add_compile_options(/utf-8 /bigobj /MP /Zc:__cplusplus)
endif()

find_package(Vulkan REQUIRED)
# GLFW 3.5.1 dal submodule deps/glfw, con una patch che aggiunge glfwStartInteractiveMove e
# glfwStartInteractiveResize (la barra del titolo disegnata dall'app, ADR 0021). Il submodule
# resta quello ufficiale: si copia nella cartella di build e la patch si applica alla copia,
# di nuovo solo quando la patch cambia. Backend Wayland e X11: l'app gira su Wayland nativo.
set(GLFW_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/tools/glfw/interactive-move-resize.patch)
set(GLFW_PATCHED ${CMAKE_BINARY_DIR}/glfw-patched)
file(SHA256 ${GLFW_PATCH} GLFW_PATCH_HASH)
if(NOT EXISTS ${GLFW_PATCHED}/.patched-${GLFW_PATCH_HASH})
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/glfw/CMakeLists.txt)
    message(FATAL_ERROR "deps/glfw is empty: run git submodule update --init")
  endif()
  file(REMOVE_RECURSE ${GLFW_PATCHED})
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/deps/glfw/ DESTINATION ${GLFW_PATCHED} PATTERN ".git" EXCLUDE)
  find_package(Git REQUIRED)
  # La copia sta dentro il repo di Lucerna: senza il limite git applicherebbe la patch rispetto
  # alla radice del repo, ignorando in silenzio i file della copia
  execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}
                  ${GIT_EXECUTABLE} apply ${GLFW_PATCH} WORKING_DIRECTORY ${GLFW_PATCHED} RESULT_VARIABLE GLFW_PATCH_RESULT)
  file(STRINGS ${GLFW_PATCHED}/include/GLFW/glfw3.h GLFW_PATCHED_API REGEX "glfwStartInteractiveResize")
  if(NOT GLFW_PATCH_RESULT EQUAL 0 OR NOT GLFW_PATCHED_API)
    message(FATAL_ERROR "The GLFW patch does not apply to deps/glfw (expected tag 3.5.1)")
  endif()
  file(TOUCH ${GLFW_PATCHED}/.patched-${GLFW_PATCH_HASH})
endif()
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${GLFW_PATCHED} ${CMAKE_BINARY_DIR}/glfw-build EXCLUDE_FROM_ALL)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_compile_definitions(GLFW_INCLUDE_NONE)

# Header visibili a tutti: glm, la radice di src (gli include si scrivono "Modulo/File.h") e i
# vendor header-only. ImGui e ImGuizmo NO: arrivano solo a chi linka il loro target.
include_directories("${SRC_DIR}/vendor/glm" "${SRC_DIR}" "${SRC_DIR}/vendor")

# ---- vendor ----------------------------------------------------------------
set(VENDOR "${SRC_DIR}/vendor")
add_library(imgui STATIC
  ${VENDOR}/imgui/imgui.cpp ${VENDOR}/imgui/imgui_demo.cpp ${VENDOR}/imgui/imgui_draw.cpp
  ${VENDOR}/imgui/imgui_tables.cpp ${VENDOR}/imgui/imgui_widgets.cpp
  ${VENDOR}/imgui/imgui_impl_glfw.cpp ${VENDOR}/imgui/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${VENDOR}/imgui)
target_compile_definitions(imgui PUBLIC ImTextureID=ImU64)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)

add_library(imguizmo STATIC ${VENDOR}/ImGuizmo/ImGuizmo.cpp)
target_include_directories(imguizmo PUBLIC ${VENDOR}/ImGuizmo)
target_link_libraries(imguizmo PUBLIC imgui)

add_library(stb STATIC ${VENDOR}/stb/stb_image_impl.cpp ${VENDOR}/stb/stb_image_write.cpp)
add_library(cgltf STATIC ${VENDOR}/cgltf_impl.cpp)

# ---- moduli ----------------------------------------------------------------
# Un modulo = una cartella di src = una libreria. DEPS sono i moduli che puo' includere:
# tools/check_modules.py fa fallire il build se un file ne include un altro (o ImGui fuori
# dall'Editor). La tabella che legge e' questa, scritta in modules.txt piu' sotto.
function(lucerna_module name)
  cmake_parse_arguments(M "" "" "DEPS;LIBS" ${ARGN})
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${SRC_DIR}/${name}/*.cpp")
  file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${SRC_DIR}/${name}/*.h")
  set(target Lucerna${name})
  if(sources)
    add_library(${target} STATIC ${sources} ${headers})
    target_precompile_headers(${target} PRIVATE "${SRC_DIR}/ptpch.h")
    set(scope PUBLIC)
  else()
    add_library(${target} INTERFACE ${headers}) # solo header
    set(scope INTERFACE)
  endif()
  foreach(dep ${M_DEPS})
    target_link_libraries(${target} ${scope} Lucerna${dep})
  endforeach()
  target_link_libraries(${target} ${scope} ${M_LIBS})
  add_dependencies(${target} ModuleCheck)
  set_property(GLOBAL APPEND_STRING PROPERTY LUCERNA_MODULES "${name}: ${M_DEPS}\n")
endfunction()

add_custom_target(ModuleCheck
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_modules.py ${SRC_DIR} ${CMAKE_BINARY_DIR}/modules.txt
  COMMENT "Controllo delle dipendenze fra moduli")

if(WIN32)
  lucerna_module(Core LIBS shell32 ole32) # SHGetKnownFolderPath, per la cartella Immagini
else()
  lucerna_module(Core)
endif()
lucerna_module(Assets        DEPS Core)
lucerna_module(Scene         DEPS Core Assets)
lucerna_module(Import        DEPS Core Assets Scene LIBS cgltf)
lucerna_module(Vulkan        DEPS Core LIBS Vulkan::Vulkan glfw stb)
lucerna_module(Render        DEPS Core Assets Scene Vulkan LIBS stb)
lucerna_module(Serialization DEPS Core Assets Scene Import Render)
lucerna_module(Editor        DEPS Core Assets Scene Import Vulkan Render Serialization LIBS imgui imguizmo glfw)

get_property(LUCERNA_MODULES GLOBAL PROPERTY LUCERNA_MODULES)
file(WRITE ${CMAKE_BINARY_DIR}/modules.txt "${LUCERNA_MODULES}")

add_executable(${PROJECT_NAME} "${SRC_DIR}/Main.cpp")

# Controlli della logica senza GPU (tests/Checks.cpp): ./build/LucernaChecks
# Document.cpp non usa ImGui: si prova senza linkare l'Editor
add_executable(LucernaChecks "${CMAKE_CURRENT_SOURCE_DIR}/tests/Checks.cpp" "${SRC_DIR}/Editor/Document.cpp" "${SRC_DIR}/Editor/BlenderLink.cpp")
target_link_libraries(LucernaChecks PRIVATE LucernaScene LucernaImport LucernaSerialization)
set_target_properties(LucernaChecks PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<0:>)
target_link_libraries(${PROJECT_NAME} PRIVATE LucernaEditor)

# Shader: SPIR-V generato a build time. glslc risolve da se' gli #include, quindi un compute
# dipende da tutti i .glsl: modificarne uno ricompila solo gli shader.
find_program(GLSLC glslc REQUIRED)
file(MAKE_DIRECTORY ${SPV_DIR})
file(GLOB_RECURSE SHADER_INCLUDES CONFIGURE_DEPENDS "${SHADER_DIR}/*.glsl")
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS "${SHADER_DIR}/*.comp")

set(SPV_FILES "")
foreach(source ${SHADER_SOURCES})
  get_filename_component(name ${source} NAME_WE)
  set(output "${SPV_DIR}/${name}.spv")
  add_custom_command(OUTPUT ${output}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -I ${SHADER_DIR} -o ${output} ${source}
    DEPENDS ${source} ${SHADER_INCLUDES}
    COMMENT "glslc ${name}.comp")
  list(APPEND SPV_FILES ${output})
endforeach()
add_custom_target(Shaders ALL DEPENDS ${SPV_FILES})
add_dependencies(${PROJECT_NAME} Shaders)

# set(CMAKE_EXE_LINKER_FLAGS "-static")



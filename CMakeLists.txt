cmake_minimum_required(VERSION 3.20)
project(SkinsAreSilly CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dear ImGui — Win32 + DX11 backend
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ---------------------------------------------------------------------------
# nlohmann/json — header-only JSON parser (used by skin_loader.h + inspect_api.h)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

add_library(imgui_win32_dx11 STATIC ${IMGUI_SOURCES})
target_include_directories(imgui_win32_dx11 PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_win32_dx11 PUBLIC
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

# ---------------------------------------------------------------------------
# cs2-dumper — auto-generated CS2 schema offsets, committed to output/
# Provides: output/client_dll.hpp, output/offsets.hpp, output/interfaces.hpp
# Updated by the community after each CS2 patch.
# https://github.com/a2x/cs2-dumper
# ---------------------------------------------------------------------------
FetchContent_Declare(
    cs2_dumper
    GIT_REPOSITORY https://github.com/a2x/cs2-dumper.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cs2_dumper)

# Always pull the latest offsets before building SovereignHook.
# cs2-dumper is updated by the community after every CS2 patch; stale offsets
# cause immediate game crashes. This target runs on every cmake --build.
add_custom_target(cs2_dumper_refresh ALL
    COMMAND git -C "${cs2_dumper_SOURCE_DIR}" fetch --depth=1 origin main
    COMMAND git -C "${cs2_dumper_SOURCE_DIR}" reset --hard FETCH_HEAD
    COMMENT "Refreshing cs2-dumper offsets from GitHub..."
    VERBATIM
)

# The generated headers live in output/ — expose that path as an include dir
set(CS2_DUMPER_INCLUDE_DIR ${cs2_dumper_SOURCE_DIR}/output)

# ---------------------------------------------------------------------------
# hl2sdk (cs2 branch) — Source 2 engine interface declarations
# Provides IEntitySystem, CEntityHandle, CBaseEntity, etc.
# https://github.com/alliedmodders/hl2sdk  branch: cs2
# ---------------------------------------------------------------------------
FetchContent_Declare(
    hl2sdk_cs2
    GIT_REPOSITORY https://github.com/alliedmodders/hl2sdk.git
    GIT_TAG        cs2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(hl2sdk_cs2)

set(HL2SDK_INCLUDE_DIRS
    ${hl2sdk_cs2_SOURCE_DIR}/public
    ${hl2sdk_cs2_SOURCE_DIR}/public/engine
    ${hl2sdk_cs2_SOURCE_DIR}/public/entity2
    ${hl2sdk_cs2_SOURCE_DIR}/common
    ${hl2sdk_cs2_SOURCE_DIR}/game/shared
)

# ---------------------------------------------------------------------------
# Target 1: SovereignHook.dll — injected into cs2.exe
# ---------------------------------------------------------------------------
add_library(SovereignHook SHARED
    src/hook/dllmain.cpp
    src/hook/sovereign_hook.cpp
    src/hook/sas_log.cpp
)
target_include_directories(SovereignHook PRIVATE
    src
    src/hook
    ${CS2_DUMPER_INCLUDE_DIR}   # client_dll.hpp, offsets.hpp, interfaces.hpp
    ${HL2SDK_INCLUDE_DIRS}      # IEntitySystem, CEntityHandle, etc.
)
target_compile_definitions(SovereignHook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # Suppress hl2sdk platform guards — we are targeting Windows x64
    COMPILER_MSVC=0
    COMPILER_GCC=1
    PLATFORM_64BITS
)
target_link_options(SovereignHook PRIVATE -static-libgcc -static-libstdc++ -static)
set_target_properties(SovereignHook PROPERTIES
    PREFIX ""
    OUTPUT_NAME "SovereignHook"
)
add_dependencies(SovereignHook cs2_dumper_refresh)

# ---------------------------------------------------------------------------
# Target 2: MuseumCurator.exe — UI + injector
# ---------------------------------------------------------------------------
add_executable(MuseumCurator WIN32
    src/main_ui.cpp
    src/injector.cpp
)
target_include_directories(MuseumCurator PRIVATE src)
target_link_libraries(MuseumCurator PRIVATE
    imgui_win32_dx11
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler_47
    dwmapi
    psapi
    winhttp
)
target_compile_definitions(MuseumCurator PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_options(MuseumCurator PRIVATE
    -static-libgcc
    -static-libstdc++
    -static
    -mwindows
)
set_target_properties(MuseumCurator PROPERTIES OUTPUT_NAME "MuseumCurator")

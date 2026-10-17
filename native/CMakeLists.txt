# weft for Godot as one GDExtension: weft's static libraries (its View, scene and DOM, QuickJS, Yoga, FreeType,
# HarfBuzz, its D3D12 and Vulkan backends), godot-cpp, and the extension's own classes (Weft, the engine singleton;
# WeftView, the Control that shows a page). Godot's Vulkan and Direct3D 12 drivers draw the pages.
#
#   cmake -S native -B <build> -DWEFT_SOURCE_DIR=<weft checkout>
#   cmake --build <build> --config Release
#
# scripts/BuildWeft.ps1 does both and copies the DLL into addons/weft/bin.
cmake_minimum_required(VERSION 3.25)
project(WeftGodot LANGUAGES C CXX)

set(WEFT_SOURCE_DIR "" CACHE PATH "The weft checkout to build")
if(NOT EXISTS "${WEFT_SOURCE_DIR}/include/weft/weft.h")
    message(FATAL_ERROR "WEFT_SOURCE_DIR (${WEFT_SOURCE_DIR}) is not a weft checkout")
endif()

# godot-cpp 10.0.0 (independent of Godot's versions), pinned by its commit and the archive's SHA256. It targets the
# GDExtension API of Godot 4.5, so the extension loads in 4.5 and later. Its MSVC runtime setting (static) is the
# whole build's: weft's libraries below take it too, and the DLL needs no Visual C++ runtime installed.
include(FetchContent)
set(GODOTCPP_API_VERSION "4.5" CACHE STRING "The Godot API the extension targets" FORCE)
set(GODOTCPP_TARGET "template_debug" CACHE STRING "godot-cpp's build target (editor and debug exports)")
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(godot-cpp
    URL https://github.com/godotengine/godot-cpp/archive/507ed9d840c01a3c5b2a39af8bb4000bfac30bf5.tar.gz
    URL_HASH SHA256=30da4ac295997061a6af81ae531510efd4e6a86e506eb4466edc0898ef3fe048)
FetchContent_MakeAvailable(godot-cpp)

# Weft's libraries only: no tests, probes or samples.
set(WEFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEFT_BUILD_PROBES OFF CACHE BOOL "" FORCE)
set(WEFT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${WEFT_SOURCE_DIR}" weft)
if(NOT TARGET weft::scene_vulkan)
    message(FATAL_ERROR "weft's Vulkan backend did not build: Godot draws with Vulkan by default (install the Vulkan SDK)")
endif()

# The commit the DLL is built from, which the extension logs when it starts ("+changes" for a checkout with changes).
execute_process(COMMAND git -C "${WEFT_SOURCE_DIR}" rev-parse HEAD OUTPUT_VARIABLE weft_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${WEFT_SOURCE_DIR}" status --porcelain --untracked-files=no OUTPUT_VARIABLE weft_dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(weft_dirty)
    string(APPEND weft_commit "+changes")
endif()

add_library(weft_godot SHARED
    src/owner_thread.cpp
    src/page_host.cpp
    src/page_source.cpp
    src/variant_pack.cpp
    src/render_thread.cpp
    src/render_bridge.cpp
    src/render_device.cpp
    src/render_vulkan.cpp
    src/render_d3d12.cpp
    src/weft_server.cpp
    src/weft_view.cpp
    src/register_types.cpp)
set_target_properties(weft_godot PROPERTIES OUTPUT_NAME "weft_godot.windows.x86_64" PREFIX "")
target_compile_features(weft_godot PRIVATE cxx_std_20)
target_include_directories(weft_godot PRIVATE src)
target_compile_definitions(weft_godot PRIVATE WEFT_GODOT_COMMIT="${weft_commit}" NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(weft_godot PRIVATE godot::cpp weft::core weft::view weft::scene_d3d12 weft::scene_vulkan weft_vulkan_headers
    d3d12 dxgi)
if(MSVC)
    # godot-cpp's headers (<godot_cpp/...>) are not held to the extension's warnings.
    target_compile_options(weft_godot PRIVATE /W4 /utf-8 /wd4100 /external:anglebrackets /external:W0)
    target_link_options(weft_godot PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

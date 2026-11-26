cmake_minimum_required(VERSION 3.20)
project(InfluClick LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Ninja, MinGW Makefiles): default to Release
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# MSVC: static runtime (same as build.bat /MT) -> no VC++ Redistributable needed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ── ImGui (same version as build.bat) ────────────────────────────────────────
set(IMGUI_VERSION "1.91.9" CACHE STRING "Dear ImGui version (git tag without 'v')")
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v${IMGUI_VERSION}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)

# ── Resources (icon is optional, manifest + version info are embedded) ───────
set(RC_FILE ${CMAKE_SOURCE_DIR}/src/app.rc)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/app.ico)
    set_source_files_properties(${RC_FILE} PROPERTIES COMPILE_DEFINITIONS HAS_ICON)
else()
    message(STATUS "src/app.ico not found - building without an application icon")
endif()

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(InfluClick WIN32
    src/main.cpp
    ${RC_FILE}
    ${IMGUI_SOURCES})

target_include_directories(InfluClick PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends)

target_link_libraries(InfluClick PRIVATE
    d3d11 dxgi dxguid winmm dwmapi)

target_compile_definitions(InfluClick PRIVATE NOMINMAX)

# ── Compiler specific options ────────────────────────────────────────────────
# No -march=native / AVX2: the exe must run on every PC that downloads it.
if(MSVC)
    target_compile_options(InfluClick PRIVATE /W3 /utf-8
        $<$<CONFIG:Release>:/O2 /GL /GS- /Gy>)
    # The manifest is embedded via app.rc -> disable the linker-generated one
    target_link_options(InfluClick PRIVATE /MANIFEST:NO /SUBSYSTEM:WINDOWS
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
else()
    # MinGW-w64 / Clang: fully static exe (no libstdc++ / winpthread DLLs needed)
    target_link_libraries(InfluClick PRIVATE d3dcompiler_47)
    target_compile_options(InfluClick PRIVATE
        $<$<CONFIG:Release>:-O2>)
    target_link_options(InfluClick PRIVATE -mwindows -static
        $<$<CONFIG:Release>:-s>)
endif()

# ── Post-build: copy to project root ─────────────────────────────────────────
add_custom_command(TARGET InfluClick POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:InfluClick>
        ${CMAKE_SOURCE_DIR}/InfluClick.exe
    COMMENT "Copying InfluClick.exe to project root")

cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(mechassault2_recomp C CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mechassault2_launcher WIN32 src/launcher.c src/app.rc)
set_source_files_properties(src/app.rc PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(mechassault2_launcher PROPERTIES
    OUTPUT_NAME "MechAssault 2"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")

set(CMAKE_C_STANDARD 11)
# clang-cl (the build under Wine/Proton, linux-proton-121): the MSVC STL from xwin wants a newer clang than LLVM 18
# (STL1000), and clang 16+ makes MSVC's implicit function declarations (warning C4013) errors. Both stay as MSVC treats
# them. Never applies to an MSVC build.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/clang:-Wno-error=implicit-function-declaration>")
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# â”€â”€ xboxrecomp toolkit â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
#
# This game project lives inside the xboxrecomp tree (MechAssault 2/),
# so the toolkit root is the parent directory.
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# â”€â”€ Recompiled game executable â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/audio_lifetime.c
    src/pc_native_settings.cpp
    src/pc_draw_distance.c
    src/pc_gui_bridge.c
    src/pc_prompts.cpp
    src/pc_storage.c
    src/bink_native.c
    # PC release additions (tank fixed step, cinematics, offline bots)
    src/ma2_tank_fixedstep.c
    src/ma2_tank_trace.c
    src/ma2_cinematic.c
    src/bots/ma2_bots_core.c
    src/bots/ma2_bot_mapid.c
    src/bots/ma2_bot_nav.c
    src/bots/ma2_bot_brain.c
    src/bots/ma2_bots_ui.c
    src/bots/ma2_bot_content.c
    src/bots/ma2_bot_terrain.c
    src/bots/ma2_bot_path.c
    # custom scenarios (.ma2map from the Scenario Editor)
    src/scenario/ma2_scenario.c
    src/scenario/ma2_scenario_audio.cpp
    src/app.rc
    vendor/imgui-1.92.5/imgui.cpp
    vendor/imgui-1.92.5/imgui_draw.cpp
    vendor/imgui-1.92.5/imgui_tables.cpp
    vendor/imgui-1.92.5/imgui_widgets.cpp
    vendor/imgui-1.92.5/backends/imgui_impl_win32.cpp
    vendor/imgui-1.92.5/backends/imgui_impl_dx11.cpp
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# ImGui displays the passive FPS overlay. Native menus and guest USB handle
# controllers independently; the overlay must not poll XInput on the renderer.
# MSVC 19.4x hit an internal compiler error (C1001) inlining in chunk 57; the shipped build
# compiles chunks 57 and 59 without inlining. Keep that: same code, and no ICE on those compilers.
set_source_files_properties(src/recomp/gen/recomp_0057.c src/recomp/gen/recomp_0059.c
    PROPERTIES COMPILE_OPTIONS /Ob0)
# The PC release links these 27 generated chunks as objects built before pipeline T1-8 switched the XMM helpers
# to SSE intrinsics (RECOMP_XMM_INTRIN 1, same results). Build them the same way, so the exe matches the release.
set_property(SOURCE src/recomp/gen/recomp_0002.c src/recomp/gen/recomp_0034.c src/recomp/gen/recomp_0043.c
    src/recomp/gen/recomp_0054.c src/recomp/gen/recomp_0057.c src/recomp/gen/recomp_0059.c src/recomp/gen/recomp_0060.c
    src/recomp/gen/recomp_0061.c src/recomp/gen/recomp_0074.c src/recomp/gen/recomp_0076.c src/recomp/gen/recomp_0080.c
    src/recomp/gen/recomp_0081.c src/recomp/gen/recomp_0082.c src/recomp/gen/recomp_0083.c src/recomp/gen/recomp_0084.c
    src/recomp/gen/recomp_0085.c src/recomp/gen/recomp_0087.c src/recomp/gen/recomp_0088.c src/recomp/gen/recomp_0089.c
    src/recomp/gen/recomp_0090.c src/recomp/gen/recomp_0091.c src/recomp/gen/recomp_0092.c src/recomp/gen/recomp_0093.c
    src/recomp/gen/recomp_0099.c src/recomp/gen/recomp_0100.c src/recomp/gen/recomp_bringup_0000.c
    src/recomp/gen/recomp_bringup_0001.c
    APPEND PROPERTY COMPILE_DEFINITIONS RECOMP_XMM_INTRIN=0)
set_property(SOURCE vendor/imgui-1.92.5/backends/imgui_impl_win32.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg-9.0.2-full_build-shared")
set_source_files_properties(src/bink_native.c PROPERTIES INCLUDE_DIRECTORIES "${FFMPEG_DIR}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE
    "${FFMPEG_DIR}/lib/avcodec.lib" "${FFMPEG_DIR}/lib/avformat.lib" "${FFMPEG_DIR}/lib/avutil.lib" "${FFMPEG_DIR}/lib/swscale.lib")
foreach(dll avcodec-63 avformat-63 avutil-61 swresample-7 swscale-10)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_DIR}/bin/${dll}.dll" "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/FFmpeg-GPL-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui-1.92.5/LICENSE.txt" "${CMAKE_CURRENT_BINARY_DIR}/ImGui-MIT-LICENSE.txt")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/src/apu/libsamplerate/COPYING"
        "${CMAKE_CURRENT_BINARY_DIR}/libsamplerate-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/src/apu/dsp/COPYING"
        "${CMAKE_CURRENT_BINARY_DIR}/DSP-GPL-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/src/apu/dsp/COPYING.LIB"
        "${CMAKE_CURRENT_BINARY_DIR}/runtime-LGPL-LICENSE.txt")

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bots
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui-1.92.5
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    foreach(application ${PROJECT_NAME} mechassault2_launcher)
        target_link_options(${application} PRIVATE /MANIFEST:EMBED
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
    endforeach()
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# â”€â”€ Game-specific additions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
option(MA2_BUILD_PC_ASSETS "Build pc_assets during the game build (the installer does it separately)" OFF)
if(MA2_BUILD_PC_ASSETS)
    find_package(Python3 COMPONENTS Interpreter)
endif()
if(MA2_BUILD_PC_ASSETS AND Python3_Interpreter_FOUND)
    set(PC_ASSET_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
        "Game root containing game_files and the pc_assets output directory")
    add_custom_command(OUTPUT "${PC_ASSET_PROJECT_DIR}/pc_assets/common.mgf"
        "${PC_ASSET_PROJECT_DIR}/pc_assets/cache.version"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/bringup/build_pc_assets.py"
            --project "${PC_ASSET_PROJECT_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bringup/build_pc_assets.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/bringup/inspect_mgf.py"
            "${PC_ASSET_PROJECT_DIR}/game_files/common.mgf"
        COMMENT "Build native PC menu forms from the configured game root"
        VERBATIM)
    add_custom_target(pc_assets DEPENDS "${PC_ASSET_PROJECT_DIR}/pc_assets/common.mgf"
        "${PC_ASSET_PROJECT_DIR}/pc_assets/cache.version")
    add_dependencies(${PROJECT_NAME} pc_assets)
    add_custom_target(pc_package
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/bringup/package_pc.py"
        DEPENDS ${PROJECT_NAME} mechassault2_launcher
        COMMENT "Create the self-contained local MechAssault 2 PC build")
endif()
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
# linux-spike-fcommon: MSVC merges C tentative definitions (g_ma2_trace_launcher in two chunks); clang needs -fcommon
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set_property(SOURCE src/recomp/gen/recomp_0021.c src/recomp/gen/recomp_fast_0021.c APPEND PROPERTY COMPILE_OPTIONS /clang:-fcommon)
endif()

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

project(defjam_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# MinGW's headers default to an older Windows than the SDK MSVC builds with,
# and hide the Windows 10 API (SetProcessDpiAwarenessContext): same floor.
if(MINGW)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Record every indirect-branch target at runtime (icall_targets.dump), so the
# targets the disassembler missed can be fed back with --seed-functions.
# Must be set BEFORE add_subdirectory so xbox_kernel's icall_feedback.c sees it.
# Remove once function detection has converged (it is a hot-path store).
add_compile_definitions(RECOMP_ICALL_FEEDBACK)

# Debug information for the runtime libraries too, not only the exe: without it
# their static functions (the whole rasteriser) have no symbol, and both the
# crash handler and RECOMP_PROFILE credit them to a neighbouring public one.
if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
endif()

# DLSS through NVIDIA Streamline (point 155): used when its SDK is in tiers/
# (downloaded apart, never committed). Its headers are C++, so C++ is enabled
# here, at the top, as CMake wants for a language a linked library uses.
if(NOT DEFINED RECOMP_STREAMLINE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tiers/streamline/include/sl.h")
    set(RECOMP_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tiers/streamline" CACHE PATH
        "NVIDIA Streamline SDK (include/, bin/x64/) for DLSS")
endif()
if(RECOMP_STREAMLINE_DIR)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

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
    src/lanceur.c
    src/pad_aleatoire.c
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

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see xboxrecomp/src/input
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)

    # Debug symbols in Release. The crash handler resolves the faulting RIP back
    # to the generated function -- i.e. to the guest function that faulted --
    # through dbghelp, and without a .pdb beside the .exe that line is simply
    # missing from every crash report. /Zi does not change code generation, so
    # the build stays a Release build; /DEBUG only writes the .pdb. Worth the
    # link time for the whole of bring-up.
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG>
                                                $<$<CONFIG:Release>:/OPT:REF>
                                                $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# ── Game-specific additions ─────────────────────────────────────
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

cmake_minimum_required(VERSION 3.14)
project(SlopPhysics VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# SDL2 is fetched and built here so the project compiles on any machine without
# a manually installed library.
include(FetchContent)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.0
)

# Static, to avoid having to copy DLLs around.
set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
FetchContent_MakeAvailable(SDL2)

# Warnings on, for our code only: this comes after SDL is added, so SDL's own
# sources keep their flags. /W4 was clean apart from six C4996 on std::fopen /
# std::sscanf. Those are silenced with the define rather than by switching to
# fopen_s, which is MSVC-flavoured Annex K and would stop the tree building
# anywhere else.
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /permissive-)
endif()

# The enemy art is checked at compile time -- every pixel connected to the
# heart, every row the right width (src/physics/body_art.h) -- and the fish's
# 94x84 frame (src/physics/fish_art.h), the largest of any species, makes that
# the largest constant expression in the build. MSVC stops evaluating a constant
# expression after 100,000 steps by default, well short of a flood fill over
# 7,896 pixels, and reports the static_assert as "not a constant expression"
# rather than as the art being wrong. GCC and Clang's defaults are far higher.
# Set after SDL so it applies to this project only.
if(MSVC)
    add_compile_options(/constexpr:steps10000000)
endif()

# AddressSanitizer, off by default: `just asan` builds it in build-asan/.
# It catches the out-of-bounds reads and use-after-free that a falling-sand grid
# indexed by hand is most likely to have, at roughly 2x run time, which is why it
# is a separate build tree and never the one grid_bench numbers come from.
#
# Placed after SDL is fetched so only our code is instrumented. MSVC's ASan
# rejects incremental linking and /RTC, so the asan recipe uses RelWithDebInfo
# (no /RTC) with incremental linking off, which still has line numbers in reports.
# Tests keep their Runs on the heap rather than this build raising the stack
# limit; tests/test_enemy.cpp says why.
option(SLOP_ASAN "Build with AddressSanitizer" OFF)
if(SLOP_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/INCREMENTAL:NO /DEBUG)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    endif()
endif()

# The source sets below are kept apart on purpose. Each one draws a
# boundary the code is not allowed to cross, and the separation is what makes a
# crossing a build error rather than a habit: the day something under
# src/physics/ reaches for the renderer, the mistake has to be written into this
# file to compile.
#
# Each set with more than one user is compiled once, as an OBJECT library, and
# linked into every target that needs it. The sets used to be listed straight
# into each add_executable, which compiled the engine once per target -- 24
# times over -- so an edit to grid.h cost about 240 engine compiles and three
# and a half minutes of a serial build.
#
# OBJECT rather than STATIC because an object library hands the linker exactly
# the object files the source list did, in the same order. A static library
# contributes only the members something references, in the order the linker
# happens to resolve them, which changes both what is linked and where it
# lands -- and grid_bench's timings move with code layout alone (see
# SLOP_BENCH_PAD below).
#
# CMake passes an object library's objects only to the targets that name it:
# a target that reaches one through another library gets its usage
# requirements but not its code. So every target names each library it uses,
# and the libraries do not link one another. That is the property the
# paragraph above asks for -- a target's link line still lists every set it
# touches, and a missing one is LNK2019 on that target. A link between two of
# these libraries would add nothing besides, since they share one include
# directory, and in the Visual Studio build it would make one library's
# compile wait for the other's.

# The simulation is deliberately free of any SDL dependency so it can be built
# and tested headlessly. Only the shell (window, input, blitting) touches SDL.
#
# slop_engine links nothing at all, so no SDL include directory can reach
# these sources; an #include <SDL.h> under src/physics/ is a compile error.
# sdl_boundary_test checks exactly that, against this library's own include
# path, so the guarantee does not rest on nobody ever adding a link here.
set(ENGINE_SOURCES
    src/physics/grid.cpp
    src/physics/topple.cpp
    src/physics/player.cpp
    src/physics/tool.cpp
    src/physics/enemy.cpp
    src/physics/arrow.cpp
    src/game/run.cpp
    src/game/level.cpp
    src/game/input_log.cpp
    src/scene/scene.cpp
)
add_library(slop_engine OBJECT ${ENGINE_SOURCES})
target_include_directories(slop_engine PUBLIC src)

# Rendering that is not SDL-side: the light field reads the grid and produces a
# plain ARGB buffer, so it is testable headlessly like the simulation is. Not
# part of ENGINE_SOURCES, because that variable's meaning is "what the
# simulation is", and light must never become a simulation input.
#
# light.cpp calls into the grid, so every target that links slop_render links
# slop_engine as well. The other two files call nothing outside this set.
set(RENDER_SOURCES
    src/render/light.cpp
    src/render/player_anim.cpp
    src/render/backdrop_set.cpp
)
add_library(slop_render OBJECT ${RENDER_SOURCES})
target_include_directories(slop_render PUBLIC src)

# Authored files turned into records: the prop list, the sprite manifest, the
# scene list and the BMP reader. Its own variable rather than a line in either
# of the two above, because it belongs to neither.
#
# Not ENGINE_SOURCES: a prop exercises no system, is never dug, ignited or
# displaced, and nothing under src/physics/ has any business reading these. Not
# RENDER_SOURCES either, since nothing here draws or knows SDL. bmp.cpp is here
# rather than inside main.cpp because the replayed benchmark row has to stamp
# the world the game actually plays in, and a loader inside the SDL shell is a
# loader no headless instrument can reach.
set(SCENE_PROP_SOURCES
    src/scene/props.cpp
    src/scene/sprites.cpp
    src/scene/bmp.cpp
    src/scene/scene_list.cpp
    src/scene/level_list.cpp
    src/scene/level_files.cpp
)
add_library(slop_scene_props OBJECT ${SCENE_PROP_SOURCES})
target_include_directories(slop_scene_props PUBLIC src)

# Frame composition and the screen-space layer. A fifth variable rather than a
# line in RENDER_SOURCES, and the difference is SDL: everything in
# RENDER_SOURCES is SDL-free by rule, and these are nothing but SDL draw calls.
# Folding them in would quietly make everything that links RENDER_SOURCES link
# SDL2 -- light_test, anim_test, grid_bench and preview_light, all of which are
# headless and must stay so.
#
# Not ENGINE_SOURCES for the reason light.cpp is not: this is rendering, and
# rendering must never become a simulation input.
#
# src/ui and render/overlay.cpp are here rather than built only into the game
# executable, which is what puts the HUD, reticle, wash and settings drawing
# inside golden_frame_test. They pass this variable's own test -- SDL draw calls
# and nothing else -- even though the name reads narrower than the contents.
#
# SDL2-static is PUBLIC: these files call SDL, and frame.h, overlay.h and the
# ui headers include SDL.h, so whoever includes them needs SDL's headers too.
set(FRAME_SOURCES
    src/render/frame.cpp
    src/render/overlay.cpp
    src/ui/text.cpp
    src/ui/hotbar.cpp
)
add_library(slop_frame OBJECT ${FRAME_SOURCES})
target_include_directories(slop_frame PUBLIC src)
target_link_libraries(slop_frame PUBLIC SDL2-static)

# The presenter: what the frame shows, worked out from a const Run -- the enemy
# atlas's pixels, the sprite lists, the HUD's words. Its own variable because it
# is the one place allowed to read both sides: the simulation (which nothing in
# src/render/ may) and the frame's record types (which nothing in src/physics/ or
# src/game/ may). It makes no SDL call, so present_test needs no window.
#
# It calls into the engine, so its users link slop_engine as well. SDL2-static
# is here for the include path only: present.h includes frame.h for the record
# types, and frame.h includes SDL.h.
set(PRESENT_SOURCES
    src/present/present.cpp
)
add_library(slop_present OBJECT ${PRESENT_SOURCES})
target_include_directories(slop_present PUBLIC src)
target_link_libraries(slop_present PUBLIC SDL2-static)

# /MP lets cl compile a library's files side by side, which matters most for
# slop_engine: every other target waits for it, and its ten files took seven
# seconds one after another. Only the libraries get it, because every other
# target here compiles one or two files and /MP has nothing to spread.
#
# MSBuild also builds several projects at once -- Directory.Build.rsp in the
# repo root passes it -m, as `cmake --build --parallel` would -- and the two
# multiply. Here the product is bounded by the libraries' 24
# files, a few seconds of a couple of compiles per hardware thread; /MP over
# the old per-target source lists would have been 20 projects of 10 to 26
# files each. /MP implies /FS, so the ASan build's /Zi PDBs are safe.
if(MSVC)
    foreach(_lib slop_engine slop_render slop_scene_props slop_frame slop_present)
        target_compile_options(${_lib} PRIVATE /MP)
    endforeach()
endif()

# The SDL shell: render targets, display modes, textures from asset files, the
# cell upload. Only the game executable links it -- everything here calls SDL,
# and every decision it used to make inline now lives behind a suite.
#
# With one user it stays a source list rather than a library: there is no
# second compile to save.
set(SHELL_SOURCES
    src/shell/shell.cpp
)

add_executable(SlopPhysics src/main.cpp ${SHELL_SOURCES})

target_include_directories(SlopPhysics PRIVATE src)

# SDL2main is required on Windows to abstract WinMain. SDL2-static is named
# here although slop_frame would bring it anyway: every target that gets SDL
# says so on its own link line, so a search for SDL2-static finds all four
# (this, golden_frame_test, present_test, preview_backdrop) and nothing else.
target_link_libraries(SlopPhysics PRIVATE slop_engine slop_render slop_scene_props slop_frame
                      slop_present SDL2main SDL2-static)

# main.cpp loads assets by a path relative to the working directory, not
# relative to the source tree, so they have to sit next to the built executable
# or a run from build/Debug/ cannot find them. Copied post-build rather than
# referenced in place so the exe stays runnable from a copied build directory.
#
# copy_directory is recursive, so assets/wip/ -- drafts, candidates and
# superseded art -- would be copied next to the exe and eventually shipped. It
# is removed immediately after rather than excluded from the copy, because
# copy_directory has no exclude and the alternatives (globbing every file, or a
# second source directory) both break the moment someone adds an asset without
# reading this. tools/load_sprite.py's staging skips directories and therefore
# already agrees; the two paths must keep agreeing or wip/ is only out of the
# build until the next full rebuild.
#
# The staged directory is deleted first, because copy_directory only adds and
# overwrites: a file renamed or removed in assets/ stayed next to the exe for
# good, and three superseded backdrop BMPs had grown the staged copy to 84 MB
# against the source's 47. Starting empty costs nothing -- copy_directory
# rewrites every file regardless, and into an empty directory it is faster
# (0.16 s against 0.47 s, measured). copy_directory_if_different would skip
# unchanged files but still never delete, and needs CMake 3.26.
add_custom_command(TARGET SlopPhysics POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:SlopPhysics>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:SlopPhysics>/assets
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:SlopPhysics>/assets/wip
)

# No test framework by design -- the harness is plain C++ and returns a non-zero
# exit code on failure, which is all CTest needs.
option(BUILD_TESTING "Build the physics test harness" ON)
if(BUILD_TESTING)
    enable_testing()
    add_executable(grid_test tests/test_grid.cpp)
    target_include_directories(grid_test PRIVATE src tests)
    target_link_libraries(grid_test PRIVATE slop_engine)
    add_test(NAME grid_test COMMAND grid_test)

    add_executable(player_test tests/test_player.cpp)
    target_include_directories(player_test PRIVATE src tests)
    target_link_libraries(player_test PRIVATE slop_engine)
    add_test(NAME player_test COMMAND player_test)

    # TUNING.md against the source it documents. Runs from the source tree because
    # the file under test lives there; links ENGINE_SOURCES only for the Player
    # constants it compares, never SDL.
    add_executable(tuning_test tests/test_tuning.cpp)
    target_include_directories(tuning_test PRIVATE src tests)
    target_link_libraries(tuning_test PRIVATE slop_engine)
    add_test(NAME tuning_test COMMAND tuning_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(tool_test tests/test_tool.cpp)
    target_include_directories(tool_test PRIVATE src tests)
    target_link_libraries(tool_test PRIVATE slop_engine)
    add_test(NAME tool_test COMMAND tool_test)

    add_executable(collapse_test tests/test_collapse.cpp)
    target_include_directories(collapse_test PRIVATE src tests)
    target_link_libraries(collapse_test PRIVATE slop_engine)
    add_test(NAME collapse_test COMMAND collapse_test)

    # Enemies and the bow: a hit takes the pixels it lands on, cuts off what it
    # severs, and every pixel lost is a grain of sand in the grid. Headless like
    # the rest of the physics suites, because the body is simulation state.
    add_executable(enemy_test tests/test_enemy.cpp)
    target_include_directories(enemy_test PRIVATE src tests)
    target_link_libraries(enemy_test PRIVATE slop_engine)
    add_test(NAME enemy_test COMMAND enemy_test)

    add_executable(run_test tests/test_run.cpp)
    target_include_directories(run_test PRIVATE src tests)
    target_link_libraries(run_test PRIVATE slop_engine)
    add_test(NAME run_test COMMAND run_test)

    # The step loop's no-allocation invariant, by counting: this executable
    # replaces global operator new, so it is its own target and links nothing
    # that would bring a second definition.
    add_executable(alloc_test tests/test_alloc.cpp)
    target_include_directories(alloc_test PRIVATE src tests)
    target_link_libraries(alloc_test PRIVATE slop_engine)
    add_test(NAME alloc_test COMMAND alloc_test)

    # The debug tooling. Links only ENGINE_SOURCES because src/game/debug_view.h
    # is a header -- it deliberately does not become another source variable, so
    # the guard the sets above enforce is untouched.
    add_executable(debug_test tests/test_debug.cpp)
    target_include_directories(debug_test PRIVATE src tests)
    target_link_libraries(debug_test PRIVATE slop_engine)
    add_test(NAME debug_test COMMAND debug_test)

    # Links the BMP reader too, and runs from the source tree, because part of
    # what this suite checks is that the shipped fixture scene still loads to
    # the cell count the session log was recorded against.
    add_executable(scene_test tests/test_scene.cpp)
    target_include_directories(scene_test PRIVATE src tests)
    target_link_libraries(scene_test PRIVATE slop_engine slop_scene_props)
    add_test(NAME scene_test COMMAND scene_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(light_test tests/test_light.cpp)
    target_include_directories(light_test PRIVATE src tests)
    target_link_libraries(light_test PRIVATE slop_engine slop_render)
    add_test(NAME light_test COMMAND light_test)

    add_executable(anim_test tests/test_anim.cpp)
    target_include_directories(anim_test PRIVATE src tests)
    target_link_libraries(anim_test PRIVATE slop_engine slop_render)
    add_test(NAME anim_test COMMAND anim_test)

    # The prop list. Links only the parser -- no engine, no SDL -- which is the
    # build file agreeing with what props.h claims about where this sits.
    add_executable(props_test tests/test_props.cpp)
    target_include_directories(props_test PRIVATE src tests)
    target_link_libraries(props_test PRIVATE slop_scene_props)
    # Run from the source tree so the suite can parse the shipped prop list,
    # which is the fixture main.cpp actually loads.
    add_test(NAME props_test COMMAND props_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # Which scenes exist. Same shape as props_test and for the same reasons -- a
    # list format whose whole contract is "reject the file, not the line" -- and
    # it runs from the source dir because its last checks parse the shipped
    # assets/scenes.txt rather than only a synthetic one.
    add_executable(scene_list_test tests/test_scene_list.cpp)
    target_include_directories(scene_list_test PRIVATE src tests)
    target_link_libraries(scene_list_test PRIVATE slop_scene_props)
    add_test(NAME scene_list_test COMMAND scene_list_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The sprite manifest. Links the same parser-only set for the same reason:
    # it turns a text file into a list of records, knows no SDL, and nothing in
    # the simulation may read it.
    add_executable(sprites_test tests/test_sprites.cpp)
    target_include_directories(sprites_test PRIVATE src tests)
    target_link_libraries(sprites_test PRIVATE slop_scene_props)
    add_test(NAME sprites_test COMMAND sprites_test)

    # The wrapping backdrop arithmetic. Links no source set at all -- the
    # narrowest link line in the project, because src/render/backdrop_wrap.h is
    # a header of pure arithmetic and nothing else. A wrapping *draw* test would
    # belong in the golden frame, not here.
    add_executable(backdrop_test tests/test_backdrop.cpp)
    target_include_directories(backdrop_test PRIVATE src tests)
    add_test(NAME backdrop_test COMMAND backdrop_test)

    # The perspective rig: the pinhole arithmetic in render/depth_rig.h.
    # Header-only and SDL-free like backdrop_test, so it links nothing.
    add_executable(rig_test tests/test_depth_rig.cpp)
    target_include_directories(rig_test PRIVATE src tests)
    add_test(NAME rig_test COMMAND rig_test)

    # Backdrop sets: the backdrop.txt parser, and every set a shipped scene names
    # checked against its BMPs -- sizes, order, feet, the plane, bands cut on flat
    # paint, the wrap seam. The parser and the BMP reader only, no SDL; from the
    # source tree so assets/ resolves.
    #
    # backdrop_set.cpp is compiled here rather than taken from slop_render,
    # because the library would bring light.cpp with it, and light.cpp calls
    # into the grid: linking it would mean linking the engine into a suite that
    # has no use for the simulation. The file does not include grid.h, so the
    # second compile is not paid when the grid changes.
    add_executable(backdrop_set_test tests/test_backdrop_set.cpp src/render/backdrop_set.cpp)
    target_include_directories(backdrop_set_test PRIVATE src tests)
    target_link_libraries(backdrop_set_test PRIVATE slop_scene_props)
    add_test(NAME backdrop_set_test COMMAND backdrop_set_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The camera's view arithmetic: the framing, the world-edge clamps, and the
    # anchor the clamps have to survive. Links no sources -- camera.h is an
    # SDL-free header.
    add_executable(camera_test tests/test_camera.cpp)
    target_include_directories(camera_test PRIVATE src tests)
    add_test(NAME camera_test COMMAND camera_test)

    # The golden frame, and the one test target that draws with SDL
    # (present_test, below, links it only for headers): it composes a real
    # frame with the real draw calls, into a software renderer over an
    # SDL_Surface. It needs no display and no video subsystem, so it still runs
    # on a build machine -- but every other suite here is headless by
    # construction and this one is not.
    #
    # SDL2main is deliberately not linked. It exists to hand Windows' WinMain to
    # a game; this has a plain main() and a console, and linking it would
    # replace that entry point.
    add_executable(golden_frame_test tests/test_golden_frame.cpp)
    target_include_directories(golden_frame_test PRIVATE src tests)
    target_link_libraries(golden_frame_test PRIVATE slop_engine slop_render slop_frame SDL2-static)
    add_test(NAME golden_frame_test COMMAND golden_frame_test)

    # The presenter, headless: the enemy atlas painted from a stepped Run and
    # checked cell by cell against what an arrow would hit there, the arrow
    # sprites, and the HUD's words. Links SDL2-static only for frame.h's record
    # types and include path; it opens nothing.
    add_executable(present_test tests/test_present.cpp)
    target_include_directories(present_test PRIVATE src tests)
    target_link_libraries(present_test PRIVATE slop_engine slop_present SDL2-static)
    add_test(NAME present_test COMMAND present_test)

    # The startup decisions: the terrain scans that plant the objective and the
    # props, and which display mode to open at. Every check in it used to run
    # inside main(), print a line to stdout, and be verifiable only by a person
    # reading that line.
    #
    # game/boot.h is a header for the same reason game/debug_view.h is, so this
    # links the two existing sets it needs and adds nothing to the build graph.
    # SCENE_PROP_SOURCES is here, and WORKING_DIRECTORY with it, because half
    # the suite runs the shipped fixture: the scene BMPs, the prop list and the
    # prop sprites.
    add_executable(boot_test tests/test_boot.cpp)
    target_include_directories(boot_test PRIVATE src tests)
    target_link_libraries(boot_test PRIVATE slop_engine slop_scene_props)
    add_test(NAME boot_test COMMAND boot_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # Starting a level: the level-file parser, the scene list's named fields and
    # level::start, plus every shipped scene started as the game starts it. From
    # the source tree for that last part, like boot_test.
    add_executable(level_test tests/test_level.cpp)
    target_include_directories(level_test PRIVATE src tests)
    target_link_libraries(level_test PRIVATE slop_engine slop_scene_props)
    add_test(NAME level_test COMMAND level_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The shell's decisions: the frame pacer's freeze rule, its step count and
    # interpolation alpha, and the settings menu's navigation and selection. All
    # of it used to live inside main()'s frame loop or inside an SDL event
    # switch, which is the definition of unreachable from a test.
    #
    # No WORKING_DIRECTORY and no SCENE_PROP_SOURCES: this suite opens no files.
    # ENGINE_SOURCES is here only because pacer.h states its step against
    # Run::FIXED_DT -- which is the point, the pacer and the physics must not be
    # able to disagree about how long a step is.
    add_executable(shell_test tests/test_shell.cpp)
    target_include_directories(shell_test PRIVATE src tests)
    target_link_libraries(shell_test PRIVATE slop_engine)
    add_test(NAME shell_test COMMAND shell_test)

    # The SDL boundary, as a test rather than a promise. The probe is one line,
    # #include <SDL.h>, compiled with slop_engine's include directories --
    # every one, including any a PRIVATE link would add -- and the test passes
    # only when that compile fails because SDL.h cannot be found. Matching the
    # message rather than WILL_FAIL is the point: a probe that fails for any
    # other reason, a typo or a missing file, must not count as the boundary
    # holding.
    #
    # It checks the include path that every file under src/physics/ is compiled
    # with. It cannot see a physics header that only SDL-side files include and
    # that includes SDL itself; nothing here could before either.
    #
    # A static library only so EXCLUDE_FROM_ALL keeps it out of the normal
    # build; it is never meant to get as far as archiving.
    add_library(sdl_boundary_probe STATIC EXCLUDE_FROM_ALL tests/sdl_boundary_probe.cpp)
    target_include_directories(sdl_boundary_probe
                               PRIVATE $<TARGET_PROPERTY:slop_engine,INCLUDE_DIRECTORIES>)
    add_test(NAME sdl_boundary_test
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                     --target sdl_boundary_probe)
    set_tests_properties(sdl_boundary_test PROPERTIES
        PASS_REGULAR_EXPRESSION "SDL\\.h'?:? (No such file|file not found)")

    # Benchmark. Deliberately not an add_test() -- timings are for reading, and
    # a slow machine should not fail the build. Links SCENE_PROP_SOURCES because
    # the replayed row rebuilds the fixture scene through the same loader the
    # game uses.
    #
    # SLOP_BENCH_PAD shifts every function linked after it by that many bytes,
    # and changes nothing else. It exists because MSVC's code layout is a source
    # of timing difference on its own: six builds of the same grid code, differing
    # only in this pad, put light/dark anywhere in a 13% band and cascading in a
    # 7% one, each build steady to ~1% run to run. An A/B whose difference is
    # inside that band can be a layout accident, and the way to tell is to rebuild
    # both sides at a second pad (16 and 32 moved things most) and see whether the
    # difference survives. 0, the default, is an empty function.
    #
    # The pad is a function of nops in its own generated source, listed first so
    # the linker places it ahead of the engine code. configure_file rather than
    # file(WRITE) so the source is only touched -- and grid_bench only relinked --
    # when the value changes.
    #
    # The libraries' objects follow a target's own sources on the link line, in
    # target_link_libraries order, so listing the pad first still puts it first:
    # it opens .text$mn, behind only the compiler's static initializers in
    # .text$di. The libraries are linked in the order their source lists used to
    # be, which keeps the rest of the layout where it was too. To check, set
    # LINK=/MAP:<file> in the environment and relink; link.exe reads it.
    set(SLOP_BENCH_PAD 0 CACHE STRING "Bytes of padding linked ahead of grid_bench's code")
    set(SLOP_PAD_BODY "")
    if(SLOP_BENCH_PAD GREATER 0)
        foreach(_i RANGE 1 ${SLOP_BENCH_PAD})
            string(APPEND SLOP_PAD_BODY " SLOP_NOP;")
        endforeach()
    endif()
    file(WRITE ${CMAKE_BINARY_DIR}/bench_pad.cpp.in
        "#if defined(_MSC_VER)\n#include <intrin.h>\n#define SLOP_NOP __nop()\n"
        "#define SLOP_NOINLINE __declspec(noinline)\n#else\n"
        "#define SLOP_NOP __asm__ volatile(\"nop\")\n"
        "#define SLOP_NOINLINE __attribute__((noinline))\n#endif\n"
        "extern \"C\" SLOP_NOINLINE void slop_bench_pad() {${SLOP_PAD_BODY} }\n")
    configure_file(${CMAKE_BINARY_DIR}/bench_pad.cpp.in ${CMAKE_BINARY_DIR}/bench_pad.cpp COPYONLY)
    add_executable(grid_bench ${CMAKE_BINARY_DIR}/bench_pad.cpp tests/bench_grid.cpp)
    target_include_directories(grid_bench PRIVATE src tests)
    target_link_libraries(grid_bench PRIVATE slop_engine slop_render slop_scene_props)

    # Headless frame preview. Not an add_test() either, and for a stronger
    # reason than the benchmark's: it asserts nothing at all. It composites
    # backdrop, cells and light exactly as main.cpp does and dumps the result,
    # so a defect that is purely a look can be examined without a window. Pipe
    # it through tools/rawpng.py.
    add_executable(preview_light tests/preview_light.cpp)
    target_include_directories(preview_light PRIVATE src tests)
    target_link_libraries(preview_light PRIVATE slop_engine slop_render)

    # The same idea for a backdrop set (assets/<dir>/backdrop.txt): stills and a
    # flight through the scene, composed by the real frame::compose. Links
    # SDL for the software renderer, as golden_frame_test does; asserts nothing.
    add_executable(preview_backdrop tests/preview_backdrop.cpp)
    target_include_directories(preview_backdrop PRIVATE src tests)
    target_link_libraries(preview_backdrop PRIVATE slop_engine slop_render slop_scene_props slop_frame
                          SDL2-static)

    # Burn timing and burn shape, in numbers. Not an add_test() for the same
    # reason preview_light is not: what it reports is a matter of taste, and a
    # test pinning today's taste would be a copy of the constants rather than a
    # check on them.
    add_executable(burn_probe tests/burn_probe.cpp)
    target_include_directories(burn_probe PRIVATE src tests)
    target_link_libraries(burn_probe PRIVATE slop_engine)

    # What the liquids do, in numbers: a U-tube, a dam break, sand poured into a
    # pool, a partial top row. Not an add_test() because what it reports is how
    # well a fluid model behaves, not whether it is correct.
    add_executable(fluid_probe tests/fluid_probe.cpp)
    target_include_directories(fluid_probe PRIVATE src tests)
    target_link_libraries(fluid_probe PRIVATE slop_engine)

    # How much of the authored rim highlight is still on a surface once the
    # world has settled. Not an add_test() for the same reason as the two above:
    # it measures an input to an art decision, not a correctness property.
    add_executable(rim_probe tests/rim_probe.cpp)
    target_include_directories(rim_probe PRIVATE src tests)
    target_link_libraries(rim_probe PRIVATE slop_engine)

    # The instrument for the Element::ticks representation question -- what a
    # second byte costs, and whether a packed integer velocity can carry a
    # gravity term. Not an add_test() for a stronger reason than the probes
    # above: every representation it flies is one the engine does not have, so a
    # test over it would pin a prototype rather than check the product.
    add_executable(velocity_probe tests/velocity_probe.cpp)
    target_include_directories(velocity_probe PRIVATE src tests)
    target_link_libraries(velocity_probe PRIVATE slop_engine)
endif()

# MSVC's ASan runtime is a DLL that lives next to cl.exe and is only on PATH
# inside a Developer prompt. Copied next to the executables so `just asan` (and
# double-clicking an ASan build) works from a plain terminal or VS Code.
# Every target here writes to the same per-config directory, so one copy covers
# the game and every test.
if(SLOP_ASAN AND MSVC)
    get_filename_component(SLOP_CL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(SLOP_ASAN_DLL "${SLOP_CL_DIR}/clang_rt.asan_dynamic-x86_64.dll")
    if(EXISTS "${SLOP_ASAN_DLL}")
        # make_directory first: this can run before any executable has created
        # the directory, and copying onto a missing directory makes a file.
        add_custom_target(asan_runtime ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SlopPhysics>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLOP_ASAN_DLL}"
                    "$<TARGET_FILE_DIR:SlopPhysics>")
    else()
        message(WARNING "SLOP_ASAN: ${SLOP_ASAN_DLL} not found; run ASan builds "
                        "from a Developer PowerShell so the runtime is on PATH.")
    endif()
endif()

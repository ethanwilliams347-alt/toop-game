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

# The five source sets below are kept apart on purpose. Each one draws a
# boundary the code is not allowed to cross, and the separation is what makes a
# crossing a build error rather than a habit: the day something under
# src/physics/ reaches for the renderer, the mistake has to be written into this
# file to compile.

# The simulation is deliberately free of any SDL dependency so it can be built
# and tested headlessly. Only the shell (window, input, blitting) touches SDL.
set(ENGINE_SOURCES
    src/physics/grid.cpp
    src/physics/topple.cpp
    src/physics/player.cpp
    src/physics/tool.cpp
    src/physics/enemy.cpp
    src/physics/arrow.cpp
    src/game/run.cpp
    src/game/input_log.cpp
    src/scene/scene.cpp
)

# Rendering that is not SDL-side: the light field reads the grid and produces a
# plain ARGB buffer, so it is testable headlessly like the simulation is. Not
# part of ENGINE_SOURCES, because that variable's meaning is "what the
# simulation is", and light must never become a simulation input.
set(RENDER_SOURCES
    src/render/light.cpp
    src/render/player_anim.cpp
    src/render/surface_plane.cpp
)

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
)

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
set(FRAME_SOURCES
    src/render/frame.cpp
    src/render/overlay.cpp
    src/ui/text.cpp
    src/ui/hotbar.cpp
)

add_executable(SlopPhysics src/main.cpp ${ENGINE_SOURCES} ${RENDER_SOURCES} ${SCENE_PROP_SOURCES} ${FRAME_SOURCES})

target_include_directories(SlopPhysics PRIVATE src)

# SDL2main is required on Windows to abstract WinMain.
target_link_libraries(SlopPhysics PRIVATE SDL2main SDL2-static)

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
add_custom_command(TARGET SlopPhysics POST_BUILD
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
    add_executable(grid_test tests/test_grid.cpp ${ENGINE_SOURCES})
    target_include_directories(grid_test PRIVATE src tests)
    add_test(NAME grid_test COMMAND grid_test)

    add_executable(player_test tests/test_player.cpp ${ENGINE_SOURCES})
    target_include_directories(player_test PRIVATE src tests)
    add_test(NAME player_test COMMAND player_test)

    add_executable(tool_test tests/test_tool.cpp ${ENGINE_SOURCES})
    target_include_directories(tool_test PRIVATE src tests)
    add_test(NAME tool_test COMMAND tool_test)

    add_executable(collapse_test tests/test_collapse.cpp ${ENGINE_SOURCES})
    target_include_directories(collapse_test PRIVATE src tests)
    add_test(NAME collapse_test COMMAND collapse_test)

    # Enemies and the bow: a hit takes the pixels it lands on, cuts off what it
    # severs, and every pixel lost is a grain of sand in the grid. Headless like
    # the rest of the physics suites, because the body is simulation state.
    add_executable(enemy_test tests/test_enemy.cpp ${ENGINE_SOURCES})
    target_include_directories(enemy_test PRIVATE src tests)
    add_test(NAME enemy_test COMMAND enemy_test)

    add_executable(run_test tests/test_run.cpp ${ENGINE_SOURCES})
    target_include_directories(run_test PRIVATE src tests)
    add_test(NAME run_test COMMAND run_test)

    # The debug tooling. Links only ENGINE_SOURCES because src/game/debug_view.h
    # is a header -- it deliberately does not become a sixth source variable, so
    # the guard the five above enforce is untouched.
    add_executable(debug_test tests/test_debug.cpp ${ENGINE_SOURCES})
    target_include_directories(debug_test PRIVATE src tests)
    add_test(NAME debug_test COMMAND debug_test)

    # Links the BMP reader too, and runs from the source tree, because part of
    # what this suite checks is that the shipped fixture scene still loads to
    # the cell count the session log was recorded against.
    add_executable(scene_test tests/test_scene.cpp ${ENGINE_SOURCES} ${SCENE_PROP_SOURCES})
    target_include_directories(scene_test PRIVATE src tests)
    add_test(NAME scene_test COMMAND scene_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(light_test tests/test_light.cpp ${ENGINE_SOURCES} ${RENDER_SOURCES})
    target_include_directories(light_test PRIVATE src tests)
    add_test(NAME light_test COMMAND light_test)

    add_executable(anim_test tests/test_anim.cpp ${ENGINE_SOURCES} ${RENDER_SOURCES})
    target_include_directories(anim_test PRIVATE src tests)
    add_test(NAME anim_test COMMAND anim_test)

    # The prop list. Links only the parser -- no engine, no SDL -- which is the
    # build file agreeing with what props.h claims about where this sits.
    add_executable(props_test tests/test_props.cpp ${SCENE_PROP_SOURCES})
    target_include_directories(props_test PRIVATE src tests)
    # Run from the source tree so the suite can parse the shipped prop list,
    # which is the fixture main.cpp actually loads.
    add_test(NAME props_test COMMAND props_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # Which scenes exist. Same shape as props_test and for the same reasons -- a
    # list format whose whole contract is "reject the file, not the line" -- and
    # it runs from the source dir because its last checks parse the shipped
    # assets/scenes.txt rather than only a synthetic one.
    add_executable(scene_list_test tests/test_scene_list.cpp ${SCENE_PROP_SOURCES})
    target_include_directories(scene_list_test PRIVATE src tests)
    add_test(NAME scene_list_test COMMAND scene_list_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The sprite manifest. Links the same parser-only set for the same reason:
    # it turns a text file into a list of records, knows no SDL, and nothing in
    # the simulation may read it.
    add_executable(sprites_test tests/test_sprites.cpp ${SCENE_PROP_SOURCES})
    target_include_directories(sprites_test PRIVATE src tests)
    add_test(NAME sprites_test COMMAND sprites_test)

    # The wrapping backdrop arithmetic. Links no source set at all -- the
    # narrowest link line in the project, because src/render/backdrop_wrap.h is
    # a header of pure arithmetic and nothing else. A wrapping *draw* test would
    # belong in the golden frame, not here.
    add_executable(backdrop_test tests/test_backdrop.cpp)
    target_include_directories(backdrop_test PRIVATE src tests)
    add_test(NAME backdrop_test COMMAND backdrop_test)

    # The near-ground pass. Links one source file rather than RENDER_SOURCES,
    # because the pass shares nothing with the light field or the sprite sheet.
    #
    # It exists because golden_frame_test structurally cannot see what it tests:
    # the pass runs between the grid's pixel buffer and SDL_UpdateTexture, and
    # the golden frame is composed from a texture that is already uploaded.
    add_executable(surface_plane_test tests/test_surface_plane.cpp
                   src/render/surface_plane.cpp)
    target_include_directories(surface_plane_test PRIVATE src tests)
    add_test(NAME surface_plane_test COMMAND surface_plane_test)

    # The perspective rig: the pinhole arithmetic in render/depth_rig.h, and every
    # rig set in render/rig_backdrop.h checked against its BMPs -- feet, plane,
    # ripple ranges, seamless wrap. Header-only and SDL-free like backdrop_test;
    # SCENE_PROP_SOURCES only for scene/bmp.cpp, and WORKING_DIRECTORY so the
    # assets resolve.
    add_executable(rig_test tests/test_depth_rig.cpp ${SCENE_PROP_SOURCES})
    target_include_directories(rig_test PRIVATE src tests)
    add_test(NAME rig_test COMMAND rig_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The camera's view arithmetic: the framing, the world-edge clamps, and the
    # anchor the clamps have to survive. Links no sources -- camera.h is an
    # SDL-free header.
    add_executable(camera_test tests/test_camera.cpp)
    target_include_directories(camera_test PRIVATE src tests)
    add_test(NAME camera_test COMMAND camera_test)

    # The golden frame, and the one test target that links SDL: it composes a
    # real frame with the real draw calls, into a software renderer over an
    # SDL_Surface. It needs no display and no video subsystem, so it still runs
    # on a build machine -- but every other suite here is headless by
    # construction and this one is not.
    #
    # SDL2main is deliberately not linked. It exists to hand Windows' WinMain to
    # a game; this has a plain main() and a console, and linking it would
    # replace that entry point.
    add_executable(golden_frame_test tests/test_golden_frame.cpp
                   ${ENGINE_SOURCES} ${RENDER_SOURCES} ${FRAME_SOURCES})
    target_include_directories(golden_frame_test PRIVATE src tests)
    target_link_libraries(golden_frame_test PRIVATE SDL2-static)
    add_test(NAME golden_frame_test COMMAND golden_frame_test)

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
    #
    # It also carries the authored backdrop's band table check, which is neither
    # a boot decision nor the fixture: it needs SCENE_PROP_SOURCES to read a BMP
    # and WORKING_DIRECTORY to find it, and this is the only suite that already
    # has both.
    add_executable(boot_test tests/test_boot.cpp ${ENGINE_SOURCES} ${SCENE_PROP_SOURCES})
    target_include_directories(boot_test PRIVATE src tests)
    add_test(NAME boot_test COMMAND boot_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The shell's decisions: the frame pacer's freeze rule, its step count and
    # interpolation alpha, and the settings menu's navigation and selection. All
    # of it used to live inside main()'s frame loop or inside an SDL event
    # switch, which is the definition of unreachable from a test.
    #
    # No WORKING_DIRECTORY and no SCENE_PROP_SOURCES: this suite opens no files.
    # ENGINE_SOURCES is here only because pacer.h states its step against
    # Run::FIXED_DT -- which is the point, the pacer and the physics must not be
    # able to disagree about how long a step is.
    add_executable(shell_test tests/test_shell.cpp ${ENGINE_SOURCES})
    target_include_directories(shell_test PRIVATE src tests)
    add_test(NAME shell_test COMMAND shell_test)

    # Benchmark. Deliberately not an add_test() -- timings are for reading, and
    # a slow machine should not fail the build. Links SCENE_PROP_SOURCES because
    # the replayed row rebuilds the fixture scene through the same loader the
    # game uses.
    add_executable(grid_bench tests/bench_grid.cpp ${ENGINE_SOURCES} ${RENDER_SOURCES} ${SCENE_PROP_SOURCES})
    target_include_directories(grid_bench PRIVATE src tests)

    # Headless frame preview. Not an add_test() either, and for a stronger
    # reason than the benchmark's: it asserts nothing at all. It composites
    # backdrop, cells and light exactly as main.cpp does and dumps the result,
    # so a defect that is purely a look can be examined without a window. Pipe
    # it through tools/rawpng.py.
    add_executable(preview_light tests/preview_light.cpp ${ENGINE_SOURCES} ${RENDER_SOURCES})
    target_include_directories(preview_light PRIVATE src tests)

    # The same idea for a perspective-rig backdrop (render/rig_backdrop.h): stills
    # and a flight through the scene, composed by the real frame::compose. Links
    # SDL for the software renderer, as golden_frame_test does; asserts nothing.
    add_executable(preview_backdrop tests/preview_backdrop.cpp
                   ${ENGINE_SOURCES} ${RENDER_SOURCES} ${SCENE_PROP_SOURCES} ${FRAME_SOURCES})
    target_include_directories(preview_backdrop PRIVATE src tests)
    target_link_libraries(preview_backdrop PRIVATE SDL2-static)

    # Burn timing and burn shape, in numbers. Not an add_test() for the same
    # reason preview_light is not: what it reports is a matter of taste, and a
    # test pinning today's taste would be a copy of the constants rather than a
    # check on them.
    add_executable(burn_probe tests/burn_probe.cpp ${ENGINE_SOURCES})
    target_include_directories(burn_probe PRIVATE src tests)

    # Where water ends up when a powder is poured into it, in numbers and in an
    # ASCII slice. Not an add_test() because the thing it reports is a shape.
    add_executable(water_probe tests/water_probe.cpp ${ENGINE_SOURCES})
    target_include_directories(water_probe PRIVATE src tests)

    # How much of the authored rim highlight is still on a surface once the
    # world has settled. Not an add_test() for the same reason as the two above:
    # it measures an input to an art decision, not a correctness property.
    add_executable(rim_probe tests/rim_probe.cpp ${ENGINE_SOURCES})
    target_include_directories(rim_probe PRIVATE src tests)

    # The instrument for the Element::ticks representation question -- what a
    # second byte costs, and whether a packed integer velocity can carry a
    # gravity term. Not an add_test() for a stronger reason than the probes
    # above: every representation it flies is one the engine does not have, so a
    # test over it would pin a prototype rather than check the product.
    add_executable(velocity_probe tests/velocity_probe.cpp ${ENGINE_SOURCES})
    target_include_directories(velocity_probe PRIVATE src tests)

    # Where the receding ground plane's band lands on screen, and how much of it
    # the fixture's own terrain covers up. Not an add_test() for the same reason
    # the four above are not: it reports an input to a composition decision.
    #
    # The one probe that reaches into src/render/, and it does so only through
    # the two headers that are arithmetic -- backdrop_layers.h and
    # backdrop_wrap.h, both header-only and SDL-free by design. RENDER_SOURCES
    # is deliberately not linked: the moment this needs a .cpp from the renderer
    # it has stopped being a headless instrument, and that should be a link
    # error rather than a habit. surface_plane.cpp is the exception, because the
    # probe reports the band ladder before and after the near-ground pass and
    # reimplementing the blend here would be a second copy of the thing being
    # measured.
    add_executable(plane_probe tests/plane_probe.cpp ${ENGINE_SOURCES}
                   src/render/surface_plane.cpp)
    target_include_directories(plane_probe PRIVATE src tests)
endif()

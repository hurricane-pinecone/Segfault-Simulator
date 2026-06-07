# Runtime tests link the full engine (SDL/OpenGL/imgui). They drive systems and
# render modules through their seams (a mock IQuadRenderer, stub services), so
# they run headless with no window or GL context. add_runtime_test(name source)
# declares one in a line.
function(add_runtime_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE engine)
    # These are plain console programs with their own main(). On Windows SDL's
    # SDL_main.h otherwise renames main -> SDL_main and SDL2main's WinMain wrapper
    # then can't find the SDL_main(int, char**) it expects (the tests take no
    # args); SDL_MAIN_HANDLED opts out of the entry-point wrapper. No-op where SDL
    # doesn't hijack main (Linux). The tests run headless and never SDL_Init, so
    # nothing needs the wrapper.
    target_compile_definitions(${name} PRIVATE SDL_MAIN_HANDLED)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS runtime)
endfunction()

# systems/
add_runtime_test(movementSystemTests systems/movementSystemTests.cpp)
add_runtime_test(collisionSystemTests systems/collisionSystemTests.cpp)
add_runtime_test(cameraSystemTests systems/cameraSystemTests.cpp)
add_runtime_test(spriteRenderSystemTests systems/spriteRenderSystemTests.cpp)

# particles/
add_runtime_test(colliderParticleCollisionTests particles/colliderParticleCollisionTests.cpp)
add_runtime_test(terrainParticleCollisionTests particles/terrainParticleCollisionTests.cpp)

# rendering/
add_runtime_test(flatRenderSmokeTest rendering/flatRenderSmokeTest.cpp)
add_runtime_test(clipDepthTests rendering/clipDepthTests.cpp)
add_runtime_test(lightingUtilsTests rendering/lightingUtilsTests.cpp)

# rendering/modules/
add_runtime_test(blockGeometryTests rendering/modules/blockGeometryTests.cpp)
add_runtime_test(isometricWaterTests rendering/modules/isometricWaterTests.cpp)
add_runtime_test(spriteShadowTests rendering/modules/spriteShadowTests.cpp)
add_runtime_test(isometricDecalSinkTests rendering/modules/isometricDecalSinkTests.cpp)

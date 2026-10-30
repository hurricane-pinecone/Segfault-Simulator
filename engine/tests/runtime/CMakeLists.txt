# Runtime tests link the full engine (SDL/imgui). They drive systems and render
# modules through their seams (a mock IRenderer3D, stub services), so they run
# headless with no window or GPU. add_runtime_test(name source)
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
add_runtime_test(physicsSystem3DTests systems/physicsSystem3DTests.cpp)

# particles/
add_runtime_test(colliderParticleCollisionTests particles/colliderParticleCollisionTests.cpp)
add_runtime_test(terrainParticleCollisionTests particles/terrainParticleCollisionTests.cpp)

# input/
add_runtime_test(keyboardInputTests input/keyboardInputTests.cpp)
add_runtime_test(mouseInputTests input/mouseInputTests.cpp)

# sceneManager/
add_runtime_test(sceneTests sceneManager/sceneTests.cpp)

# rendering/

# rendering/modules/

# assetStore/
add_runtime_test(modelImporterTests assetStore/modelImporterTests.cpp)

# rendering3d/
add_runtime_test(renderSystem3DTests rendering3d/renderSystem3DTests.cpp)
add_runtime_test(scene2DTests rendering3d/scene2DTests.cpp)
add_runtime_test(uiLayoutTests ui/uiLayoutTests.cpp)
add_runtime_test(pickingTests rendering3d/pickingTests.cpp)

# serialization/
add_runtime_test(sceneSerializerTests serialization/sceneSerializerTests.cpp)
add_runtime_test(prefabTests serialization/prefabTests.cpp)

# scripting/
add_runtime_test(scriptRegistryTests scripting/scriptRegistryTests.cpp)

# scriptableObjects/
add_runtime_test(scriptableObjectsTests scriptableObjects/scriptableObjectsTests.cpp)
add_runtime_test(renderBackendsTests rendering/renderBackendsTests.cpp)

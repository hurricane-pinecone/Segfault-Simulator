# Editor unit tests for code that doesn't need a window (protocol parsing,
# models). Each test compiles the editor sources it covers.
set(EDITOR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

function(add_editor_test name source)
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${EDITOR_SRC_DIR}"
        "${PROJECT_SOURCE_DIR}/engine/lib/glm")
    # Console programs with their own main(), as the runtime tests are (see
    # engine/tests/runtime/CMakeLists.txt): an engine header that includes SDL
    # would otherwise rename main for SDL2main's wrapper on Windows.
    target_compile_definitions(${name} PRIVATE SDL_MAIN_HANDLED)
    if(WIN32)
        target_sources(${name} PRIVATE
            "${PROJECT_SOURCE_DIR}/engine/tests/runtime/sdlMainStub.cpp")
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS editor)
endfunction()

add_editor_test(msgpackTests msgpackTests.cpp "${EDITOR_SRC_DIR}/nvim/msgpack.cpp")
add_editor_test(nvimScreenTests nvimScreenTests.cpp "${EDITOR_SRC_DIR}/nvim/nvimScreen.cpp" "${EDITOR_SRC_DIR}/nvim/msgpack.cpp")

add_editor_test(nvimClientTests nvimClientTests.cpp
    "${EDITOR_SRC_DIR}/nvim/nvimClient.cpp"
    "${EDITOR_SRC_DIR}/nvim/nvimScreen.cpp"
    "${EDITOR_SRC_DIR}/nvim/msgpack.cpp"
    "${EDITOR_SRC_DIR}/build/childProcess.cpp"
    "${EDITOR_SRC_DIR}/build/buildEnvironment.cpp")
target_include_directories(nvimClientTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../generated")
target_compile_definitions(nvimClientTests PRIVATE
    SFS_TEST_NVIM_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../nvim")
target_link_libraries(nvimClientTests PRIVATE sfs::engine-core)
# The engine config starts the built sfs-lsp (SFS_EDITOR_LSP) in its tests.
if(TARGET sfs-lsp)
    add_dependencies(nvimClientTests sfs-lsp)
endif()

add_editor_test(projectFilesTests projectFilesTests.cpp "${EDITOR_SRC_DIR}/project/project.cpp")
target_link_libraries(projectFilesTests PRIVATE sfs::engine-core)
# The SFS tree's own .clangd must say what a project's says about attributes.
target_compile_definitions(projectFilesTests PRIVATE SFS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_editor_test(gameExporterTests gameExporterTests.cpp
    "${EDITOR_SRC_DIR}/build/gameExporter.cpp"
    "${EDITOR_SRC_DIR}/project/project.cpp")
target_link_libraries(gameExporterTests PRIVATE sfs::engine)

add_editor_test(fieldMigrationTests fieldMigrationTests.cpp "${EDITOR_SRC_DIR}/scripting/fieldMigration.cpp")
target_link_libraries(fieldMigrationTests PRIVATE sfs::engine-core)

add_editor_test(gltfFilesTests gltfFilesTests.cpp "${EDITOR_SRC_DIR}/assets/gltfFiles.cpp")
target_link_libraries(gltfFilesTests PRIVATE sfs::engine-core)

add_editor_test(historyStepsTests historyStepsTests.cpp "${EDITOR_SRC_DIR}/history/historySteps.cpp")
target_link_libraries(historyStepsTests PRIVATE sfs::engine-core)

add_editor_test(nodeIdUpgradeTests nodeIdUpgradeTests.cpp "${EDITOR_SRC_DIR}/build/nodeIdUpgrade.cpp")
target_link_libraries(nodeIdUpgradeTests PRIVATE sfs::engine)

add_editor_test(referencesTests referencesTests.cpp
    "${EDITOR_SRC_DIR}/scene/references.cpp"
    "${EDITOR_SRC_DIR}/scene/parts.cpp"
    "${EDITOR_SRC_DIR}/scene/unusedOverrides.cpp"
    "${EDITOR_SRC_DIR}/build/fillParts.cpp")
target_link_libraries(referencesTests PRIVATE sfs::engine)

add_editor_test(prefabsTests prefabsTests.cpp
    "${EDITOR_SRC_DIR}/scene/prefabs.cpp"
    "${EDITOR_SRC_DIR}/scene/editorScene.cpp"
    "${EDITOR_SRC_DIR}/scene/sceneDocument.cpp"
    "${EDITOR_SRC_DIR}/scene/references.cpp"
    "${EDITOR_SRC_DIR}/scene/parts.cpp"
    "${EDITOR_SRC_DIR}/scene/unusedOverrides.cpp"
    "${EDITOR_SRC_DIR}/history/editorHistory.cpp"
    "${EDITOR_SRC_DIR}/history/historySteps.cpp")
target_link_libraries(prefabsTests PRIVATE sfs::engine)

add_editor_test(codeFontTests codeFontTests.cpp "${EDITOR_SRC_DIR}/widgets/codeFont.cpp")
target_link_libraries(codeFontTests PRIVATE sfs::engine)
target_compile_definitions(codeFontTests PRIVATE
    SFS_TEST_FONTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fonts")

add_editor_test(widgetsTests widgetsTests.cpp "${EDITOR_SRC_DIR}/widgets/widgets.cpp")
target_link_libraries(widgetsTests PRIVATE sfs::engine)

add_editor_test(panelTests panelTests.cpp
    "${EDITOR_SRC_DIR}/panels/consolePanel.cpp"
    "${EDITOR_SRC_DIR}/panels/panel.cpp"
    "${EDITOR_SRC_DIR}/widgets/widgets.cpp")
target_link_libraries(panelTests PRIVATE sfs::engine)

add_editor_test(uiPreviewTests uiPreviewTests.cpp "${EDITOR_SRC_DIR}/panels/uiPreviewDraw.cpp")
target_link_libraries(uiPreviewTests PRIVATE sfs::engine)

add_editor_test(boardCanvasTests boardCanvasTests.cpp "${EDITOR_SRC_DIR}/widgets/boardCanvas.cpp")
target_link_libraries(boardCanvasTests PRIVATE sfs::engine)

add_editor_test(architectureTests architectureTests.cpp
    "${EDITOR_SRC_DIR}/architecture/graph.cpp"
    "${EDITOR_SRC_DIR}/architecture/highlight.cpp"
    "${EDITOR_SRC_DIR}/architecture/layout.cpp"
    "${EDITOR_SRC_DIR}/architecture/positions.cpp"
    "${EDITOR_SRC_DIR}/architecture/routing.cpp"
    "${EDITOR_SRC_DIR}/architecture/generator.cpp"
    "${EDITOR_SRC_DIR}/build/childProcess.cpp")
target_link_libraries(architectureTests PRIVATE sfs::engine)

add_editor_test(selectionTests selectionTests.cpp)
target_link_libraries(selectionTests PRIVATE sfs::engine)

if(NOT WIN32)
    add_editor_test(childProcessTests childProcessTests.cpp
        "${EDITOR_SRC_DIR}/build/childProcess.cpp"
        "${EDITOR_SRC_DIR}/build/buildEnvironment.cpp")
    target_include_directories(childProcessTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../generated")
    target_link_libraries(childProcessTests PRIVATE sfs::engine-core)
endif()

add_editor_test(scriptFilesTests scriptFilesTests.cpp "${EDITOR_SRC_DIR}/scripting/scriptFiles.cpp"
    "${EDITOR_SRC_DIR}/platform/paths.cpp")
target_link_libraries(scriptFilesTests PRIVATE sfs::engine-core)

add_editor_test(moduleBuilderTests moduleBuilderTests.cpp
    "${EDITOR_SRC_DIR}/build/moduleBuilder.cpp"
    "${EDITOR_SRC_DIR}/build/buildEnvironment.cpp"
    "${EDITOR_SRC_DIR}/build/childProcess.cpp"
    "${EDITOR_SRC_DIR}/project/project.cpp")
target_include_directories(moduleBuilderTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../generated")
target_link_libraries(moduleBuilderTests PRIVATE sfs::engine)

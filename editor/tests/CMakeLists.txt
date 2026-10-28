# Editor unit tests for code that doesn't need a window (protocol parsing,
# models). Each test compiles the editor sources it covers.
set(EDITOR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

function(add_editor_test name source)
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${EDITOR_SRC_DIR}"
        "${PROJECT_SOURCE_DIR}/engine/lib/glm")
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

add_editor_test(gameExporterTests gameExporterTests.cpp
    "${EDITOR_SRC_DIR}/build/gameExporter.cpp"
    "${EDITOR_SRC_DIR}/project/project.cpp")
target_link_libraries(gameExporterTests PRIVATE sfs::engine)

add_editor_test(fieldMigrationTests fieldMigrationTests.cpp "${EDITOR_SRC_DIR}/scripting/fieldMigration.cpp")
target_link_libraries(fieldMigrationTests PRIVATE sfs::engine-core)

add_editor_test(gltfFilesTests gltfFilesTests.cpp "${EDITOR_SRC_DIR}/assets/gltfFiles.cpp")
target_link_libraries(gltfFilesTests PRIVATE sfs::engine-core)

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

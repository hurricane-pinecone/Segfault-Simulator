# sfs-lsp: the SFS language server (completion and checks of [[sfs::...]]
# attributes, codegen's diagnostics live, and lints of the SFS API), run by an
# editor beside clangd. See docs/editor/sfs-lsp.

# The protocol's transport (framing, JSON-RPC, URIs, position encodings), a
# library of its own so the engine's LSP clients can share it.
add_library(sfs-lsp-protocol STATIC protocol/src/protocol.cpp)
target_include_directories(sfs-lsp-protocol PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/include"
    "${PROJECT_SOURCE_DIR}/engine/lib" # nlohmann/json
)
target_compile_options(sfs-lsp-protocol PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>)

# The server, as a library the tests link too.
add_library(sfs-lsp-lib STATIC
    src/analysis.cpp
    src/check.cpp
    src/codeActions.cpp
    src/completion.cpp
    src/document.cpp
    src/entityLints.cpp
    src/hover.cpp
    src/lints.cpp
    src/projectIndex.cpp
    src/server.cpp
    # --fix writes files the way the engine does (engine-core's, with the
    # logger it reports through).
    "${PROJECT_SOURCE_DIR}/engine/src/core/util/atomicFile.cpp"
    "${PROJECT_SOURCE_DIR}/engine/src/core/logger/logger.cpp"
)
target_include_directories(sfs-lsp-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(sfs-lsp-lib PRIVATE "${PROJECT_SOURCE_DIR}/engine/include")
target_link_libraries(sfs-lsp-lib PUBLIC sfs-codegen-lib sfs-lsp-protocol)
target_compile_definitions(sfs-lsp-lib PUBLIC SFS_ENGINE_VERSION="${ENGINE_VERSION}")
target_compile_options(sfs-lsp-lib PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>)
find_package(Threads REQUIRED)
target_link_libraries(sfs-lsp-lib PUBLIC Threads::Threads)

add_executable(sfs-lsp src/main.cpp)
target_link_libraries(sfs-lsp PRIVATE sfs-lsp-lib)
target_compile_options(sfs-lsp PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>)
set_target_properties(sfs-lsp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# One file on Windows: no MinGW runtime DLLs to ship beside it.
if(MINGW)
    target_link_options(sfs-lsp PRIVATE -static)
endif()

# Installed with the engine (bin/, and the editor's SDK), beside sfs-codegen.
install(TARGETS sfs-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

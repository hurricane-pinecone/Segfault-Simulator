# sfs-codegen: generates reflection registration from script headers. A build
# (host) tool: cross-builds use a host copy instead (SFS_CODEGEN_EXECUTABLE).
find_package(tree-sitter REQUIRED)
find_package(tree-sitter-cpp REQUIRED)

add_library(sfs-codegen-lib STATIC
    src/parser.cpp
    src/generator.cpp
)
target_include_directories(sfs-codegen-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(sfs-codegen-lib PRIVATE
    tree-sitter-cpp::tree-sitter-cpp
    tree-sitter::tree-sitter
)
target_compile_options(sfs-codegen-lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
)

add_executable(sfs-codegen src/main.cpp)
target_link_libraries(sfs-codegen PRIVATE sfs-codegen-lib)
set_target_properties(sfs-codegen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Installed with the engine, so a project built against the package can run it.
install(TARGETS sfs-codegen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

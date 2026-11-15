# Build-tool tests: sfs-codegen's parser and generator, and sfs-lsp (only where
# the tools are built natively).
add_executable(codegenTests codegenTests.cpp)
target_link_libraries(codegenTests PRIVATE sfs-codegen-lib)
# The shared test harness prints glm vectors.
target_include_directories(codegenTests PRIVATE "${PROJECT_SOURCE_DIR}/engine/lib/glm")
# The CLI test runs the built sfs-codegen; the docs tests read the docs pages.
target_compile_definitions(codegenTests PRIVATE
    SFS_CODEGEN_PATH="$<TARGET_FILE:sfs-codegen>"
    SFS_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
add_dependencies(codegenTests sfs-codegen)
add_test(NAME codegenTests COMMAND codegenTests)
set_tests_properties(codegenTests PROPERTIES LABELS tools)

# sfs-lsp: documents and encodings, completion and hover, lints and code
# actions, and the server over pipes (POSIX: it spawns the built sfs-lsp).
function(add_lsp_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE sfs-lsp-lib)
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/engine/lib/glm")
    target_compile_definitions(${name} PRIVATE
        SFS_LSP_PATH="$<TARGET_FILE:sfs-lsp>"
        SFS_LSP_TEST_SOURCE="${PROJECT_SOURCE_DIR}/engine/tools/codegen/src/parser.cpp")
    add_dependencies(${name} sfs-lsp)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS tools)
endfunction()
add_lsp_test(lspDocumentTests)
add_lsp_test(lspCompletionTests)
add_lsp_test(lspLintTests)
if(NOT WIN32)
    add_lsp_test(lspProtocolTests)
endif()

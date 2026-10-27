# Build-tool tests: sfs-codegen's parser and generator (only where the tool is
# built, i.e. native non-core-only builds).
add_executable(codegenTests codegenTests.cpp)
target_link_libraries(codegenTests PRIVATE sfs-codegen-lib)
# The shared test harness prints glm vectors.
target_include_directories(codegenTests PRIVATE "${PROJECT_SOURCE_DIR}/engine/lib/glm")
add_test(NAME codegenTests COMMAND codegenTests)
set_tests_properties(codegenTests PROPERTIES LABELS tools)

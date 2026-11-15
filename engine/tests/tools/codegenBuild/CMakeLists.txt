# A project using sfsCodegen.cmake with a stand-in codegen built in it, as a
# native build's is, for codegenBuildTest.cmake.
cmake_minimum_required(VERSION 3.20)
project(codegenBuildFixture CXX)
add_executable(sfs-codegen fakegen.cpp)
set(SFS_CODEGEN_COMMAND sfs-codegen)
set(SFS_CODEGEN_DEPENDS sfs-codegen)
include("${SFS_CODEGEN_CMAKE}")

add_library(module OBJECT module.cpp)
sfs_generate_reflection(module
    MODULE Fixture
    FUNCTION fixtureRegister
    FUNCTION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/register.h"
    HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/register.h"
    EVENT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp")

if(WITH_TESTS)
    add_library(tests OBJECT tests.cpp)
    sfs_event_traits(tests FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests.cpp")
endif()

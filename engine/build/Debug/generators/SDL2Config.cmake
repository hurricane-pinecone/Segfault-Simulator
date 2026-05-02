########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(SDL2_FIND_QUIETLY)
    set(SDL2_MESSAGE_MODE VERBOSE)
else()
    set(SDL2_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/SDL2Targets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${sdl_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(SDL2_VERSION_STRING "2.28.3")
set(SDL2_INCLUDE_DIRS ${sdl_INCLUDE_DIRS_DEBUG} )
set(SDL2_INCLUDE_DIR ${sdl_INCLUDE_DIRS_DEBUG} )
set(SDL2_LIBRARIES ${sdl_LIBRARIES_DEBUG} )
set(SDL2_DEFINITIONS ${sdl_DEFINITIONS_DEBUG} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${sdl_BUILD_MODULES_PATHS_DEBUG} )
    message(${SDL2_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()



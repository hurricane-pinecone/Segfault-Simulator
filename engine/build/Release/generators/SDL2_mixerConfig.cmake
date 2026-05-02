########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(SDL2_mixer_FIND_QUIETLY)
    set(SDL2_mixer_MESSAGE_MODE VERBOSE)
else()
    set(SDL2_mixer_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/SDL2_mixerTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${sdl_mixer_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(SDL2_mixer_VERSION_STRING "2.8.0")
set(SDL2_mixer_INCLUDE_DIRS ${sdl_mixer_INCLUDE_DIRS_RELEASE} )
set(SDL2_mixer_INCLUDE_DIR ${sdl_mixer_INCLUDE_DIRS_RELEASE} )
set(SDL2_mixer_LIBRARIES ${sdl_mixer_LIBRARIES_RELEASE} )
set(SDL2_mixer_DEFINITIONS ${sdl_mixer_DEFINITIONS_RELEASE} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${sdl_mixer_BUILD_MODULES_PATHS_RELEASE} )
    message(${SDL2_mixer_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()



########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(SDL2_image_FIND_QUIETLY)
    set(SDL2_image_MESSAGE_MODE VERBOSE)
else()
    set(SDL2_image_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/SDL2_imageTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${sdl_image_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(SDL2_image_VERSION_STRING "2.8.2")
set(SDL2_image_INCLUDE_DIRS ${sdl_image_INCLUDE_DIRS_DEBUG} )
set(SDL2_image_INCLUDE_DIR ${sdl_image_INCLUDE_DIRS_DEBUG} )
set(SDL2_image_LIBRARIES ${sdl_image_LIBRARIES_DEBUG} )
set(SDL2_image_DEFINITIONS ${sdl_image_DEFINITIONS_DEBUG} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${sdl_image_BUILD_MODULES_PATHS_DEBUG} )
    message(${SDL2_image_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()



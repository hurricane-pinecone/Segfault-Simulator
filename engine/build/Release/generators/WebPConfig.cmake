########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(WebP_FIND_QUIETLY)
    set(WebP_MESSAGE_MODE VERBOSE)
else()
    set(WebP_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/WebPTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${libwebp_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(WebP_VERSION_STRING "1.6.0")
set(WebP_INCLUDE_DIRS ${libwebp_INCLUDE_DIRS_RELEASE} )
set(WebP_INCLUDE_DIR ${libwebp_INCLUDE_DIRS_RELEASE} )
set(WebP_LIBRARIES ${libwebp_LIBRARIES_RELEASE} )
set(WebP_DEFINITIONS ${libwebp_DEFINITIONS_RELEASE} )

set(WEBP_VERSION_STRING "1.6.0")
set(WEBP_INCLUDE_DIRS ${libwebp_INCLUDE_DIRS_RELEASE} )
set(WEBP_INCLUDE_DIR ${libwebp_INCLUDE_DIRS_RELEASE} )
set(WEBP_LIBRARIES ${libwebp_LIBRARIES_RELEASE} )
set(WEBP_DEFINITIONS ${libwebp_DEFINITIONS_RELEASE} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${libwebp_BUILD_MODULES_PATHS_RELEASE} )
    message(${WebP_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()



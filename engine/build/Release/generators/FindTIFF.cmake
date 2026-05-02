########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(TIFF_FIND_QUIETLY)
    set(TIFF_MESSAGE_MODE VERBOSE)
else()
    set(TIFF_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/module-TIFFTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${libtiff_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(TIFF_VERSION_STRING "4.6.0")
set(TIFF_INCLUDE_DIRS ${libtiff_INCLUDE_DIRS_RELEASE} )
set(TIFF_INCLUDE_DIR ${libtiff_INCLUDE_DIRS_RELEASE} )
set(TIFF_LIBRARIES ${libtiff_LIBRARIES_RELEASE} )
set(TIFF_DEFINITIONS ${libtiff_DEFINITIONS_RELEASE} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${libtiff_BUILD_MODULES_PATHS_RELEASE} )
    message(${TIFF_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()


include(FindPackageHandleStandardArgs)
set(TIFF_FOUND 1)
set(TIFF_VERSION "4.6.0")

find_package_handle_standard_args(TIFF
                                  REQUIRED_VARS TIFF_VERSION
                                  VERSION_VAR TIFF_VERSION)
mark_as_advanced(TIFF_FOUND TIFF_VERSION)

set(TIFF_FOUND 1)
set(TIFF_VERSION "4.6.0")
mark_as_advanced(TIFF_FOUND TIFF_VERSION)


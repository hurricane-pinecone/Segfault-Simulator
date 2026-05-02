########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(libmodplug_FIND_QUIETLY)
    set(libmodplug_MESSAGE_MODE VERBOSE)
else()
    set(libmodplug_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/libmodplugTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${libmodplug_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(libmodplug_VERSION_STRING "0.8.9.0")
set(libmodplug_INCLUDE_DIRS ${libmodplug_INCLUDE_DIRS_DEBUG} )
set(libmodplug_INCLUDE_DIR ${libmodplug_INCLUDE_DIRS_DEBUG} )
set(libmodplug_LIBRARIES ${libmodplug_LIBRARIES_DEBUG} )
set(libmodplug_DEFINITIONS ${libmodplug_DEFINITIONS_DEBUG} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${libmodplug_BUILD_MODULES_PATHS_DEBUG} )
    message(${libmodplug_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()



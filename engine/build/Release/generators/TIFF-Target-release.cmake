# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(libtiff_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(libtiff_FRAMEWORKS_FOUND_RELEASE "${libtiff_FRAMEWORKS_RELEASE}" "${libtiff_FRAMEWORK_DIRS_RELEASE}")

set(libtiff_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET libtiff_DEPS_TARGET)
    add_library(libtiff_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET libtiff_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${libtiff_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${libtiff_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:ZLIB::ZLIB;LibLZMA::LibLZMA;JPEG::JPEG>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### libtiff_DEPS_TARGET to all of them
conan_package_library_targets("${libtiff_LIBS_RELEASE}"    # libraries
                              "${libtiff_LIB_DIRS_RELEASE}" # package_libdir
                              "${libtiff_BIN_DIRS_RELEASE}" # package_bindir
                              "${libtiff_LIBRARY_TYPE_RELEASE}"
                              "${libtiff_IS_HOST_WINDOWS_RELEASE}"
                              libtiff_DEPS_TARGET
                              libtiff_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "libtiff"    # package_name
                              "${libtiff_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${libtiff_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## GLOBAL TARGET PROPERTIES Release ########################################
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 $<$<CONFIG:Release>:${libtiff_OBJECTS_RELEASE}>
                 $<$<CONFIG:Release>:${libtiff_LIBRARIES_TARGETS}>
                 )

    if("${libtiff_LIBS_RELEASE}" STREQUAL "")
        # If the package is not declaring any "cpp_info.libs" the package deps, system libs,
        # frameworks etc are not linked to the imported targets and we need to do it to the
        # global target
        set_property(TARGET TIFF::TIFF
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     libtiff_DEPS_TARGET)
    endif()

    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                 $<$<CONFIG:Release>:${libtiff_LINKER_FLAGS_RELEASE}>)
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                 $<$<CONFIG:Release>:${libtiff_INCLUDE_DIRS_RELEASE}>)
    # Necessary to find LINK shared libraries in Linux
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                 $<$<CONFIG:Release>:${libtiff_LIB_DIRS_RELEASE}>)
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                 $<$<CONFIG:Release>:${libtiff_COMPILE_DEFINITIONS_RELEASE}>)
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                 $<$<CONFIG:Release>:${libtiff_COMPILE_OPTIONS_RELEASE}>)

########## For the modules (FindXXX)
set(libtiff_LIBRARIES_RELEASE TIFF::TIFF)

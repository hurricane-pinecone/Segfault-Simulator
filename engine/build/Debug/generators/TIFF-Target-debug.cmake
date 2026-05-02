# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(libtiff_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(libtiff_FRAMEWORKS_FOUND_DEBUG "${libtiff_FRAMEWORKS_DEBUG}" "${libtiff_FRAMEWORK_DIRS_DEBUG}")

set(libtiff_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET libtiff_DEPS_TARGET)
    add_library(libtiff_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET libtiff_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${libtiff_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${libtiff_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:ZLIB::ZLIB;LibLZMA::LibLZMA;JPEG::JPEG>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### libtiff_DEPS_TARGET to all of them
conan_package_library_targets("${libtiff_LIBS_DEBUG}"    # libraries
                              "${libtiff_LIB_DIRS_DEBUG}" # package_libdir
                              "${libtiff_BIN_DIRS_DEBUG}" # package_bindir
                              "${libtiff_LIBRARY_TYPE_DEBUG}"
                              "${libtiff_IS_HOST_WINDOWS_DEBUG}"
                              libtiff_DEPS_TARGET
                              libtiff_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "libtiff"    # package_name
                              "${libtiff_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${libtiff_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## GLOBAL TARGET PROPERTIES Debug ########################################
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 $<$<CONFIG:Debug>:${libtiff_OBJECTS_DEBUG}>
                 $<$<CONFIG:Debug>:${libtiff_LIBRARIES_TARGETS}>
                 )

    if("${libtiff_LIBS_DEBUG}" STREQUAL "")
        # If the package is not declaring any "cpp_info.libs" the package deps, system libs,
        # frameworks etc are not linked to the imported targets and we need to do it to the
        # global target
        set_property(TARGET TIFF::TIFF
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     libtiff_DEPS_TARGET)
    endif()

    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                 $<$<CONFIG:Debug>:${libtiff_LINKER_FLAGS_DEBUG}>)
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                 $<$<CONFIG:Debug>:${libtiff_INCLUDE_DIRS_DEBUG}>)
    # Necessary to find LINK shared libraries in Linux
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                 $<$<CONFIG:Debug>:${libtiff_LIB_DIRS_DEBUG}>)
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                 $<$<CONFIG:Debug>:${libtiff_COMPILE_DEFINITIONS_DEBUG}>)
    set_property(TARGET TIFF::TIFF
                 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                 $<$<CONFIG:Debug>:${libtiff_COMPILE_OPTIONS_DEBUG}>)

########## For the modules (FindXXX)
set(libtiff_LIBRARIES_DEBUG TIFF::TIFF)

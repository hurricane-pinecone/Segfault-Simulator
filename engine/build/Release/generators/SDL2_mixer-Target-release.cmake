# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(sdl_mixer_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(sdl_mixer_FRAMEWORKS_FOUND_RELEASE "${sdl_mixer_FRAMEWORKS_RELEASE}" "${sdl_mixer_FRAMEWORK_DIRS_RELEASE}")

set(sdl_mixer_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET sdl_mixer_DEPS_TARGET)
    add_library(sdl_mixer_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET sdl_mixer_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${sdl_mixer_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${sdl_mixer_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:SDL2::SDL2main;flac::flac;mpg123::mpg123;opusfile::opusfile;libmodplug::libmodplug;Ogg::ogg>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### sdl_mixer_DEPS_TARGET to all of them
conan_package_library_targets("${sdl_mixer_LIBS_RELEASE}"    # libraries
                              "${sdl_mixer_LIB_DIRS_RELEASE}" # package_libdir
                              "${sdl_mixer_BIN_DIRS_RELEASE}" # package_bindir
                              "${sdl_mixer_LIBRARY_TYPE_RELEASE}"
                              "${sdl_mixer_IS_HOST_WINDOWS_RELEASE}"
                              sdl_mixer_DEPS_TARGET
                              sdl_mixer_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "sdl_mixer"    # package_name
                              "${sdl_mixer_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${sdl_mixer_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## GLOBAL TARGET PROPERTIES Release ########################################
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 $<$<CONFIG:Release>:${sdl_mixer_OBJECTS_RELEASE}>
                 $<$<CONFIG:Release>:${sdl_mixer_LIBRARIES_TARGETS}>
                 )

    if("${sdl_mixer_LIBS_RELEASE}" STREQUAL "")
        # If the package is not declaring any "cpp_info.libs" the package deps, system libs,
        # frameworks etc are not linked to the imported targets and we need to do it to the
        # global target
        set_property(TARGET SDL2_mixer::SDL2_mixer
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     sdl_mixer_DEPS_TARGET)
    endif()

    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                 $<$<CONFIG:Release>:${sdl_mixer_LINKER_FLAGS_RELEASE}>)
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                 $<$<CONFIG:Release>:${sdl_mixer_INCLUDE_DIRS_RELEASE}>)
    # Necessary to find LINK shared libraries in Linux
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                 $<$<CONFIG:Release>:${sdl_mixer_LIB_DIRS_RELEASE}>)
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                 $<$<CONFIG:Release>:${sdl_mixer_COMPILE_DEFINITIONS_RELEASE}>)
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                 $<$<CONFIG:Release>:${sdl_mixer_COMPILE_OPTIONS_RELEASE}>)

########## For the modules (FindXXX)
set(sdl_mixer_LIBRARIES_RELEASE SDL2_mixer::SDL2_mixer)

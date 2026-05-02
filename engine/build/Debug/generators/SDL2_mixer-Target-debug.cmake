# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(sdl_mixer_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(sdl_mixer_FRAMEWORKS_FOUND_DEBUG "${sdl_mixer_FRAMEWORKS_DEBUG}" "${sdl_mixer_FRAMEWORK_DIRS_DEBUG}")

set(sdl_mixer_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET sdl_mixer_DEPS_TARGET)
    add_library(sdl_mixer_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET sdl_mixer_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${sdl_mixer_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${sdl_mixer_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:SDL2::SDL2main;flac::flac;mpg123::mpg123;opusfile::opusfile;libmodplug::libmodplug;Ogg::ogg>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### sdl_mixer_DEPS_TARGET to all of them
conan_package_library_targets("${sdl_mixer_LIBS_DEBUG}"    # libraries
                              "${sdl_mixer_LIB_DIRS_DEBUG}" # package_libdir
                              "${sdl_mixer_BIN_DIRS_DEBUG}" # package_bindir
                              "${sdl_mixer_LIBRARY_TYPE_DEBUG}"
                              "${sdl_mixer_IS_HOST_WINDOWS_DEBUG}"
                              sdl_mixer_DEPS_TARGET
                              sdl_mixer_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "sdl_mixer"    # package_name
                              "${sdl_mixer_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${sdl_mixer_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## GLOBAL TARGET PROPERTIES Debug ########################################
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 $<$<CONFIG:Debug>:${sdl_mixer_OBJECTS_DEBUG}>
                 $<$<CONFIG:Debug>:${sdl_mixer_LIBRARIES_TARGETS}>
                 )

    if("${sdl_mixer_LIBS_DEBUG}" STREQUAL "")
        # If the package is not declaring any "cpp_info.libs" the package deps, system libs,
        # frameworks etc are not linked to the imported targets and we need to do it to the
        # global target
        set_property(TARGET SDL2_mixer::SDL2_mixer
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     sdl_mixer_DEPS_TARGET)
    endif()

    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                 $<$<CONFIG:Debug>:${sdl_mixer_LINKER_FLAGS_DEBUG}>)
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                 $<$<CONFIG:Debug>:${sdl_mixer_INCLUDE_DIRS_DEBUG}>)
    # Necessary to find LINK shared libraries in Linux
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                 $<$<CONFIG:Debug>:${sdl_mixer_LIB_DIRS_DEBUG}>)
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                 $<$<CONFIG:Debug>:${sdl_mixer_COMPILE_DEFINITIONS_DEBUG}>)
    set_property(TARGET SDL2_mixer::SDL2_mixer
                 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                 $<$<CONFIG:Debug>:${sdl_mixer_COMPILE_OPTIONS_DEBUG}>)

########## For the modules (FindXXX)
set(sdl_mixer_LIBRARIES_DEBUG SDL2_mixer::SDL2_mixer)

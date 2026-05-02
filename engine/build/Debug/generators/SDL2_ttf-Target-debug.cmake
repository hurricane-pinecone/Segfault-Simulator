# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(sdl_ttf_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(sdl_ttf_FRAMEWORKS_FOUND_DEBUG "${sdl_ttf_FRAMEWORKS_DEBUG}" "${sdl_ttf_FRAMEWORK_DIRS_DEBUG}")

set(sdl_ttf_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET sdl_ttf_DEPS_TARGET)
    add_library(sdl_ttf_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET sdl_ttf_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${sdl_ttf_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${sdl_ttf_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:Freetype::Freetype;SDL2::SDL2>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### sdl_ttf_DEPS_TARGET to all of them
conan_package_library_targets("${sdl_ttf_LIBS_DEBUG}"    # libraries
                              "${sdl_ttf_LIB_DIRS_DEBUG}" # package_libdir
                              "${sdl_ttf_BIN_DIRS_DEBUG}" # package_bindir
                              "${sdl_ttf_LIBRARY_TYPE_DEBUG}"
                              "${sdl_ttf_IS_HOST_WINDOWS_DEBUG}"
                              sdl_ttf_DEPS_TARGET
                              sdl_ttf_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "sdl_ttf"    # package_name
                              "${sdl_ttf_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${sdl_ttf_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Debug ########################################

    ########## COMPONENT SDL2_ttf::SDL2_ttf-static #############

        set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORKS_FOUND_DEBUG "")
        conan_find_apple_frameworks(sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORKS_FOUND_DEBUG "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORKS_DEBUG}" "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORK_DIRS_DEBUG}")

        set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPS_TARGET)
            add_library(sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORKS_FOUND_DEBUG}>
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_SYSTEM_LIBS_DEBUG}>
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPENDENCIES_DEBUG}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPS_TARGET' to all of them
        conan_package_library_targets("${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBS_DEBUG}"
                              "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIB_DIRS_DEBUG}"
                              "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_BIN_DIRS_DEBUG}" # package_bindir
                              "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBRARY_TYPE_DEBUG}"
                              "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_IS_HOST_WINDOWS_DEBUG}"
                              sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPS_TARGET
                              sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBRARIES_TARGETS
                              "_DEBUG"
                              "sdl_ttf_SDL2_ttf_SDL2_ttf-static"
                              "${sdl_ttf_SDL2_ttf_SDL2_ttf-static_NO_SONAME_MODE_DEBUG}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET SDL2_ttf::SDL2_ttf-static
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_OBJECTS_DEBUG}>
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBRARIES_TARGETS}>
                     )

        if("${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBS_DEBUG}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET SDL2_ttf::SDL2_ttf-static
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPS_TARGET)
        endif()

        set_property(TARGET SDL2_ttf::SDL2_ttf-static APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LINKER_FLAGS_DEBUG}>)
        set_property(TARGET SDL2_ttf::SDL2_ttf-static APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_INCLUDE_DIRS_DEBUG}>)
        set_property(TARGET SDL2_ttf::SDL2_ttf-static APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIB_DIRS_DEBUG}>)
        set_property(TARGET SDL2_ttf::SDL2_ttf-static APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_DEFINITIONS_DEBUG}>)
        set_property(TARGET SDL2_ttf::SDL2_ttf-static APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Debug>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_OPTIONS_DEBUG}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET sdl_ttf::sdl_ttf APPEND PROPERTY INTERFACE_LINK_LIBRARIES SDL2_ttf::SDL2_ttf-static)

########## For the modules (FindXXX)
set(sdl_ttf_LIBRARIES_DEBUG sdl_ttf::sdl_ttf)

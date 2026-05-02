# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(sdl_image_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(sdl_image_FRAMEWORKS_FOUND_DEBUG "${sdl_image_FRAMEWORKS_DEBUG}" "${sdl_image_FRAMEWORK_DIRS_DEBUG}")

set(sdl_image_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET sdl_image_DEPS_TARGET)
    add_library(sdl_image_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET sdl_image_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${sdl_image_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${sdl_image_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:SDL2::SDL2main;TIFF::TIFF;JPEG::JPEG;PNG::PNG;libwebp::libwebp>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### sdl_image_DEPS_TARGET to all of them
conan_package_library_targets("${sdl_image_LIBS_DEBUG}"    # libraries
                              "${sdl_image_LIB_DIRS_DEBUG}" # package_libdir
                              "${sdl_image_BIN_DIRS_DEBUG}" # package_bindir
                              "${sdl_image_LIBRARY_TYPE_DEBUG}"
                              "${sdl_image_IS_HOST_WINDOWS_DEBUG}"
                              sdl_image_DEPS_TARGET
                              sdl_image_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "sdl_image"    # package_name
                              "${sdl_image_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${sdl_image_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Debug ########################################

    ########## COMPONENT SDL2_image::SDL2_image #############

        set(sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_FOUND_DEBUG "")
        conan_find_apple_frameworks(sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_FOUND_DEBUG "${sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_DEBUG}" "${sdl_image_SDL2_image_SDL2_image_FRAMEWORK_DIRS_DEBUG}")

        set(sdl_image_SDL2_image_SDL2_image_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET sdl_image_SDL2_image_SDL2_image_DEPS_TARGET)
            add_library(sdl_image_SDL2_image_SDL2_image_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET sdl_image_SDL2_image_SDL2_image_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_FOUND_DEBUG}>
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_SYSTEM_LIBS_DEBUG}>
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_DEPENDENCIES_DEBUG}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'sdl_image_SDL2_image_SDL2_image_DEPS_TARGET' to all of them
        conan_package_library_targets("${sdl_image_SDL2_image_SDL2_image_LIBS_DEBUG}"
                              "${sdl_image_SDL2_image_SDL2_image_LIB_DIRS_DEBUG}"
                              "${sdl_image_SDL2_image_SDL2_image_BIN_DIRS_DEBUG}" # package_bindir
                              "${sdl_image_SDL2_image_SDL2_image_LIBRARY_TYPE_DEBUG}"
                              "${sdl_image_SDL2_image_SDL2_image_IS_HOST_WINDOWS_DEBUG}"
                              sdl_image_SDL2_image_SDL2_image_DEPS_TARGET
                              sdl_image_SDL2_image_SDL2_image_LIBRARIES_TARGETS
                              "_DEBUG"
                              "sdl_image_SDL2_image_SDL2_image"
                              "${sdl_image_SDL2_image_SDL2_image_NO_SONAME_MODE_DEBUG}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET SDL2_image::SDL2_image
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_OBJECTS_DEBUG}>
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_LIBRARIES_TARGETS}>
                     )

        if("${sdl_image_SDL2_image_SDL2_image_LIBS_DEBUG}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET SDL2_image::SDL2_image
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         sdl_image_SDL2_image_SDL2_image_DEPS_TARGET)
        endif()

        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_LINKER_FLAGS_DEBUG}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_INCLUDE_DIRS_DEBUG}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_LIB_DIRS_DEBUG}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_COMPILE_DEFINITIONS_DEBUG}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Debug>:${sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_DEBUG}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_LINK_LIBRARIES SDL2_image::SDL2_image)

########## For the modules (FindXXX)
set(sdl_image_LIBRARIES_DEBUG SDL2_image::SDL2_image)

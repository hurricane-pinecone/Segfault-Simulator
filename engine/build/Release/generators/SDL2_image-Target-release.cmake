# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(sdl_image_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(sdl_image_FRAMEWORKS_FOUND_RELEASE "${sdl_image_FRAMEWORKS_RELEASE}" "${sdl_image_FRAMEWORK_DIRS_RELEASE}")

set(sdl_image_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET sdl_image_DEPS_TARGET)
    add_library(sdl_image_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET sdl_image_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${sdl_image_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${sdl_image_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:SDL2::SDL2main;TIFF::TIFF;JPEG::JPEG;PNG::PNG;libwebp::libwebp>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### sdl_image_DEPS_TARGET to all of them
conan_package_library_targets("${sdl_image_LIBS_RELEASE}"    # libraries
                              "${sdl_image_LIB_DIRS_RELEASE}" # package_libdir
                              "${sdl_image_BIN_DIRS_RELEASE}" # package_bindir
                              "${sdl_image_LIBRARY_TYPE_RELEASE}"
                              "${sdl_image_IS_HOST_WINDOWS_RELEASE}"
                              sdl_image_DEPS_TARGET
                              sdl_image_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "sdl_image"    # package_name
                              "${sdl_image_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${sdl_image_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Release ########################################

    ########## COMPONENT SDL2_image::SDL2_image #############

        set(sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_FOUND_RELEASE "${sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_RELEASE}" "${sdl_image_SDL2_image_SDL2_image_FRAMEWORK_DIRS_RELEASE}")

        set(sdl_image_SDL2_image_SDL2_image_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET sdl_image_SDL2_image_SDL2_image_DEPS_TARGET)
            add_library(sdl_image_SDL2_image_SDL2_image_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET sdl_image_SDL2_image_SDL2_image_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'sdl_image_SDL2_image_SDL2_image_DEPS_TARGET' to all of them
        conan_package_library_targets("${sdl_image_SDL2_image_SDL2_image_LIBS_RELEASE}"
                              "${sdl_image_SDL2_image_SDL2_image_LIB_DIRS_RELEASE}"
                              "${sdl_image_SDL2_image_SDL2_image_BIN_DIRS_RELEASE}" # package_bindir
                              "${sdl_image_SDL2_image_SDL2_image_LIBRARY_TYPE_RELEASE}"
                              "${sdl_image_SDL2_image_SDL2_image_IS_HOST_WINDOWS_RELEASE}"
                              sdl_image_SDL2_image_SDL2_image_DEPS_TARGET
                              sdl_image_SDL2_image_SDL2_image_LIBRARIES_TARGETS
                              "_RELEASE"
                              "sdl_image_SDL2_image_SDL2_image"
                              "${sdl_image_SDL2_image_SDL2_image_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET SDL2_image::SDL2_image
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_LIBRARIES_TARGETS}>
                     )

        if("${sdl_image_SDL2_image_SDL2_image_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET SDL2_image::SDL2_image
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         sdl_image_SDL2_image_SDL2_image_DEPS_TARGET)
        endif()

        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_LIB_DIRS_RELEASE}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_RELEASE}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET SDL2_image::SDL2_image APPEND PROPERTY INTERFACE_LINK_LIBRARIES SDL2_image::SDL2_image)

########## For the modules (FindXXX)
set(sdl_image_LIBRARIES_RELEASE SDL2_image::SDL2_image)

# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(sdl_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(sdl_FRAMEWORKS_FOUND_RELEASE "${sdl_FRAMEWORKS_RELEASE}" "${sdl_FRAMEWORK_DIRS_RELEASE}")

set(sdl_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET sdl_DEPS_TARGET)
    add_library(sdl_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET sdl_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${sdl_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${sdl_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:SDL2::SDL2>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### sdl_DEPS_TARGET to all of them
conan_package_library_targets("${sdl_LIBS_RELEASE}"    # libraries
                              "${sdl_LIB_DIRS_RELEASE}" # package_libdir
                              "${sdl_BIN_DIRS_RELEASE}" # package_bindir
                              "${sdl_LIBRARY_TYPE_RELEASE}"
                              "${sdl_IS_HOST_WINDOWS_RELEASE}"
                              sdl_DEPS_TARGET
                              sdl_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "sdl"    # package_name
                              "${sdl_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${sdl_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Release ########################################

    ########## COMPONENT SDL2::SDL2main #############

        set(sdl_SDL2_SDL2main_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(sdl_SDL2_SDL2main_FRAMEWORKS_FOUND_RELEASE "${sdl_SDL2_SDL2main_FRAMEWORKS_RELEASE}" "${sdl_SDL2_SDL2main_FRAMEWORK_DIRS_RELEASE}")

        set(sdl_SDL2_SDL2main_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET sdl_SDL2_SDL2main_DEPS_TARGET)
            add_library(sdl_SDL2_SDL2main_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET sdl_SDL2_SDL2main_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'sdl_SDL2_SDL2main_DEPS_TARGET' to all of them
        conan_package_library_targets("${sdl_SDL2_SDL2main_LIBS_RELEASE}"
                              "${sdl_SDL2_SDL2main_LIB_DIRS_RELEASE}"
                              "${sdl_SDL2_SDL2main_BIN_DIRS_RELEASE}" # package_bindir
                              "${sdl_SDL2_SDL2main_LIBRARY_TYPE_RELEASE}"
                              "${sdl_SDL2_SDL2main_IS_HOST_WINDOWS_RELEASE}"
                              sdl_SDL2_SDL2main_DEPS_TARGET
                              sdl_SDL2_SDL2main_LIBRARIES_TARGETS
                              "_RELEASE"
                              "sdl_SDL2_SDL2main"
                              "${sdl_SDL2_SDL2main_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET SDL2::SDL2main
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_LIBRARIES_TARGETS}>
                     )

        if("${sdl_SDL2_SDL2main_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET SDL2::SDL2main
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         sdl_SDL2_SDL2main_DEPS_TARGET)
        endif()

        set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_LIB_DIRS_RELEASE}>)
        set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2main_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT SDL2::SDL2 #############

        set(sdl_SDL2_SDL2_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(sdl_SDL2_SDL2_FRAMEWORKS_FOUND_RELEASE "${sdl_SDL2_SDL2_FRAMEWORKS_RELEASE}" "${sdl_SDL2_SDL2_FRAMEWORK_DIRS_RELEASE}")

        set(sdl_SDL2_SDL2_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET sdl_SDL2_SDL2_DEPS_TARGET)
            add_library(sdl_SDL2_SDL2_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET sdl_SDL2_SDL2_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'sdl_SDL2_SDL2_DEPS_TARGET' to all of them
        conan_package_library_targets("${sdl_SDL2_SDL2_LIBS_RELEASE}"
                              "${sdl_SDL2_SDL2_LIB_DIRS_RELEASE}"
                              "${sdl_SDL2_SDL2_BIN_DIRS_RELEASE}" # package_bindir
                              "${sdl_SDL2_SDL2_LIBRARY_TYPE_RELEASE}"
                              "${sdl_SDL2_SDL2_IS_HOST_WINDOWS_RELEASE}"
                              sdl_SDL2_SDL2_DEPS_TARGET
                              sdl_SDL2_SDL2_LIBRARIES_TARGETS
                              "_RELEASE"
                              "sdl_SDL2_SDL2"
                              "${sdl_SDL2_SDL2_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET SDL2::SDL2
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_LIBRARIES_TARGETS}>
                     )

        if("${sdl_SDL2_SDL2_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET SDL2::SDL2
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         sdl_SDL2_SDL2_DEPS_TARGET)
        endif()

        set_property(TARGET SDL2::SDL2 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET SDL2::SDL2 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET SDL2::SDL2 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_LIB_DIRS_RELEASE}>)
        set_property(TARGET SDL2::SDL2 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET SDL2::SDL2 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${sdl_SDL2_SDL2_COMPILE_OPTIONS_RELEASE}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_LINK_LIBRARIES SDL2::SDL2main)
    set_property(TARGET SDL2::SDL2main APPEND PROPERTY INTERFACE_LINK_LIBRARIES SDL2::SDL2)

########## For the modules (FindXXX)
set(sdl_LIBRARIES_RELEASE SDL2::SDL2main)

# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(opusfile_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(opusfile_FRAMEWORKS_FOUND_RELEASE "${opusfile_FRAMEWORKS_RELEASE}" "${opusfile_FRAMEWORK_DIRS_RELEASE}")

set(opusfile_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET opusfile_DEPS_TARGET)
    add_library(opusfile_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET opusfile_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${opusfile_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${opusfile_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:Ogg::ogg;Opus::opus;opusfile::libopusfile;openssl::openssl>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### opusfile_DEPS_TARGET to all of them
conan_package_library_targets("${opusfile_LIBS_RELEASE}"    # libraries
                              "${opusfile_LIB_DIRS_RELEASE}" # package_libdir
                              "${opusfile_BIN_DIRS_RELEASE}" # package_bindir
                              "${opusfile_LIBRARY_TYPE_RELEASE}"
                              "${opusfile_IS_HOST_WINDOWS_RELEASE}"
                              opusfile_DEPS_TARGET
                              opusfile_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "opusfile"    # package_name
                              "${opusfile_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${opusfile_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Release ########################################

    ########## COMPONENT opusfile::opusurl #############

        set(opusfile_opusfile_opusurl_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(opusfile_opusfile_opusurl_FRAMEWORKS_FOUND_RELEASE "${opusfile_opusfile_opusurl_FRAMEWORKS_RELEASE}" "${opusfile_opusfile_opusurl_FRAMEWORK_DIRS_RELEASE}")

        set(opusfile_opusfile_opusurl_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET opusfile_opusfile_opusurl_DEPS_TARGET)
            add_library(opusfile_opusfile_opusurl_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET opusfile_opusfile_opusurl_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'opusfile_opusfile_opusurl_DEPS_TARGET' to all of them
        conan_package_library_targets("${opusfile_opusfile_opusurl_LIBS_RELEASE}"
                              "${opusfile_opusfile_opusurl_LIB_DIRS_RELEASE}"
                              "${opusfile_opusfile_opusurl_BIN_DIRS_RELEASE}" # package_bindir
                              "${opusfile_opusfile_opusurl_LIBRARY_TYPE_RELEASE}"
                              "${opusfile_opusfile_opusurl_IS_HOST_WINDOWS_RELEASE}"
                              opusfile_opusfile_opusurl_DEPS_TARGET
                              opusfile_opusfile_opusurl_LIBRARIES_TARGETS
                              "_RELEASE"
                              "opusfile_opusfile_opusurl"
                              "${opusfile_opusfile_opusurl_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET opusfile::opusurl
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_LIBRARIES_TARGETS}>
                     )

        if("${opusfile_opusfile_opusurl_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET opusfile::opusurl
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         opusfile_opusfile_opusurl_DEPS_TARGET)
        endif()

        set_property(TARGET opusfile::opusurl APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET opusfile::opusurl APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET opusfile::opusurl APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_LIB_DIRS_RELEASE}>)
        set_property(TARGET opusfile::opusurl APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET opusfile::opusurl APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${opusfile_opusfile_opusurl_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT opusfile::libopusfile #############

        set(opusfile_opusfile_libopusfile_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(opusfile_opusfile_libopusfile_FRAMEWORKS_FOUND_RELEASE "${opusfile_opusfile_libopusfile_FRAMEWORKS_RELEASE}" "${opusfile_opusfile_libopusfile_FRAMEWORK_DIRS_RELEASE}")

        set(opusfile_opusfile_libopusfile_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET opusfile_opusfile_libopusfile_DEPS_TARGET)
            add_library(opusfile_opusfile_libopusfile_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET opusfile_opusfile_libopusfile_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'opusfile_opusfile_libopusfile_DEPS_TARGET' to all of them
        conan_package_library_targets("${opusfile_opusfile_libopusfile_LIBS_RELEASE}"
                              "${opusfile_opusfile_libopusfile_LIB_DIRS_RELEASE}"
                              "${opusfile_opusfile_libopusfile_BIN_DIRS_RELEASE}" # package_bindir
                              "${opusfile_opusfile_libopusfile_LIBRARY_TYPE_RELEASE}"
                              "${opusfile_opusfile_libopusfile_IS_HOST_WINDOWS_RELEASE}"
                              opusfile_opusfile_libopusfile_DEPS_TARGET
                              opusfile_opusfile_libopusfile_LIBRARIES_TARGETS
                              "_RELEASE"
                              "opusfile_opusfile_libopusfile"
                              "${opusfile_opusfile_libopusfile_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET opusfile::libopusfile
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_LIBRARIES_TARGETS}>
                     )

        if("${opusfile_opusfile_libopusfile_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET opusfile::libopusfile
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         opusfile_opusfile_libopusfile_DEPS_TARGET)
        endif()

        set_property(TARGET opusfile::libopusfile APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET opusfile::libopusfile APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET opusfile::libopusfile APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_LIB_DIRS_RELEASE}>)
        set_property(TARGET opusfile::libopusfile APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET opusfile::libopusfile APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${opusfile_opusfile_libopusfile_COMPILE_OPTIONS_RELEASE}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET opusfile::opusfile APPEND PROPERTY INTERFACE_LINK_LIBRARIES opusfile::opusurl)
    set_property(TARGET opusfile::opusfile APPEND PROPERTY INTERFACE_LINK_LIBRARIES opusfile::libopusfile)

########## For the modules (FindXXX)
set(opusfile_LIBRARIES_RELEASE opusfile::opusfile)

# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(libwebp_FRAMEWORKS_FOUND_RELEASE "") # Will be filled later
conan_find_apple_frameworks(libwebp_FRAMEWORKS_FOUND_RELEASE "${libwebp_FRAMEWORKS_RELEASE}" "${libwebp_FRAMEWORK_DIRS_RELEASE}")

set(libwebp_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET libwebp_DEPS_TARGET)
    add_library(libwebp_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET libwebp_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Release>:${libwebp_FRAMEWORKS_FOUND_RELEASE}>
             $<$<CONFIG:Release>:${libwebp_SYSTEM_LIBS_RELEASE}>
             $<$<CONFIG:Release>:WebP::sharpyuv;WebP::webp>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### libwebp_DEPS_TARGET to all of them
conan_package_library_targets("${libwebp_LIBS_RELEASE}"    # libraries
                              "${libwebp_LIB_DIRS_RELEASE}" # package_libdir
                              "${libwebp_BIN_DIRS_RELEASE}" # package_bindir
                              "${libwebp_LIBRARY_TYPE_RELEASE}"
                              "${libwebp_IS_HOST_WINDOWS_RELEASE}"
                              libwebp_DEPS_TARGET
                              libwebp_LIBRARIES_TARGETS  # out_libraries_targets
                              "_RELEASE"
                              "libwebp"    # package_name
                              "${libwebp_NO_SONAME_MODE_RELEASE}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${libwebp_BUILD_DIRS_RELEASE} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Release ########################################

    ########## COMPONENT WebP::libwebpmux #############

        set(libwebp_WebP_libwebpmux_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libwebp_WebP_libwebpmux_FRAMEWORKS_FOUND_RELEASE "${libwebp_WebP_libwebpmux_FRAMEWORKS_RELEASE}" "${libwebp_WebP_libwebpmux_FRAMEWORK_DIRS_RELEASE}")

        set(libwebp_WebP_libwebpmux_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libwebp_WebP_libwebpmux_DEPS_TARGET)
            add_library(libwebp_WebP_libwebpmux_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libwebp_WebP_libwebpmux_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libwebp_WebP_libwebpmux_DEPS_TARGET' to all of them
        conan_package_library_targets("${libwebp_WebP_libwebpmux_LIBS_RELEASE}"
                              "${libwebp_WebP_libwebpmux_LIB_DIRS_RELEASE}"
                              "${libwebp_WebP_libwebpmux_BIN_DIRS_RELEASE}" # package_bindir
                              "${libwebp_WebP_libwebpmux_LIBRARY_TYPE_RELEASE}"
                              "${libwebp_WebP_libwebpmux_IS_HOST_WINDOWS_RELEASE}"
                              libwebp_WebP_libwebpmux_DEPS_TARGET
                              libwebp_WebP_libwebpmux_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libwebp_WebP_libwebpmux"
                              "${libwebp_WebP_libwebpmux_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET WebP::libwebpmux
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_LIBRARIES_TARGETS}>
                     )

        if("${libwebp_WebP_libwebpmux_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET WebP::libwebpmux
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libwebp_WebP_libwebpmux_DEPS_TARGET)
        endif()

        set_property(TARGET WebP::libwebpmux APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET WebP::libwebpmux APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET WebP::libwebpmux APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_LIB_DIRS_RELEASE}>)
        set_property(TARGET WebP::libwebpmux APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET WebP::libwebpmux APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_libwebpmux_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT WebP::webpdemux #############

        set(libwebp_WebP_webpdemux_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libwebp_WebP_webpdemux_FRAMEWORKS_FOUND_RELEASE "${libwebp_WebP_webpdemux_FRAMEWORKS_RELEASE}" "${libwebp_WebP_webpdemux_FRAMEWORK_DIRS_RELEASE}")

        set(libwebp_WebP_webpdemux_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libwebp_WebP_webpdemux_DEPS_TARGET)
            add_library(libwebp_WebP_webpdemux_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libwebp_WebP_webpdemux_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libwebp_WebP_webpdemux_DEPS_TARGET' to all of them
        conan_package_library_targets("${libwebp_WebP_webpdemux_LIBS_RELEASE}"
                              "${libwebp_WebP_webpdemux_LIB_DIRS_RELEASE}"
                              "${libwebp_WebP_webpdemux_BIN_DIRS_RELEASE}" # package_bindir
                              "${libwebp_WebP_webpdemux_LIBRARY_TYPE_RELEASE}"
                              "${libwebp_WebP_webpdemux_IS_HOST_WINDOWS_RELEASE}"
                              libwebp_WebP_webpdemux_DEPS_TARGET
                              libwebp_WebP_webpdemux_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libwebp_WebP_webpdemux"
                              "${libwebp_WebP_webpdemux_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET WebP::webpdemux
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_LIBRARIES_TARGETS}>
                     )

        if("${libwebp_WebP_webpdemux_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET WebP::webpdemux
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libwebp_WebP_webpdemux_DEPS_TARGET)
        endif()

        set_property(TARGET WebP::webpdemux APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET WebP::webpdemux APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET WebP::webpdemux APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_LIB_DIRS_RELEASE}>)
        set_property(TARGET WebP::webpdemux APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET WebP::webpdemux APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdemux_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT WebP::webp #############

        set(libwebp_WebP_webp_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libwebp_WebP_webp_FRAMEWORKS_FOUND_RELEASE "${libwebp_WebP_webp_FRAMEWORKS_RELEASE}" "${libwebp_WebP_webp_FRAMEWORK_DIRS_RELEASE}")

        set(libwebp_WebP_webp_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libwebp_WebP_webp_DEPS_TARGET)
            add_library(libwebp_WebP_webp_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libwebp_WebP_webp_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libwebp_WebP_webp_DEPS_TARGET' to all of them
        conan_package_library_targets("${libwebp_WebP_webp_LIBS_RELEASE}"
                              "${libwebp_WebP_webp_LIB_DIRS_RELEASE}"
                              "${libwebp_WebP_webp_BIN_DIRS_RELEASE}" # package_bindir
                              "${libwebp_WebP_webp_LIBRARY_TYPE_RELEASE}"
                              "${libwebp_WebP_webp_IS_HOST_WINDOWS_RELEASE}"
                              libwebp_WebP_webp_DEPS_TARGET
                              libwebp_WebP_webp_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libwebp_WebP_webp"
                              "${libwebp_WebP_webp_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET WebP::webp
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_LIBRARIES_TARGETS}>
                     )

        if("${libwebp_WebP_webp_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET WebP::webp
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libwebp_WebP_webp_DEPS_TARGET)
        endif()

        set_property(TARGET WebP::webp APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET WebP::webp APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET WebP::webp APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_LIB_DIRS_RELEASE}>)
        set_property(TARGET WebP::webp APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET WebP::webp APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webp_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT WebP::sharpyuv #############

        set(libwebp_WebP_sharpyuv_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libwebp_WebP_sharpyuv_FRAMEWORKS_FOUND_RELEASE "${libwebp_WebP_sharpyuv_FRAMEWORKS_RELEASE}" "${libwebp_WebP_sharpyuv_FRAMEWORK_DIRS_RELEASE}")

        set(libwebp_WebP_sharpyuv_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libwebp_WebP_sharpyuv_DEPS_TARGET)
            add_library(libwebp_WebP_sharpyuv_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libwebp_WebP_sharpyuv_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libwebp_WebP_sharpyuv_DEPS_TARGET' to all of them
        conan_package_library_targets("${libwebp_WebP_sharpyuv_LIBS_RELEASE}"
                              "${libwebp_WebP_sharpyuv_LIB_DIRS_RELEASE}"
                              "${libwebp_WebP_sharpyuv_BIN_DIRS_RELEASE}" # package_bindir
                              "${libwebp_WebP_sharpyuv_LIBRARY_TYPE_RELEASE}"
                              "${libwebp_WebP_sharpyuv_IS_HOST_WINDOWS_RELEASE}"
                              libwebp_WebP_sharpyuv_DEPS_TARGET
                              libwebp_WebP_sharpyuv_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libwebp_WebP_sharpyuv"
                              "${libwebp_WebP_sharpyuv_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET WebP::sharpyuv
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_LIBRARIES_TARGETS}>
                     )

        if("${libwebp_WebP_sharpyuv_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET WebP::sharpyuv
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libwebp_WebP_sharpyuv_DEPS_TARGET)
        endif()

        set_property(TARGET WebP::sharpyuv APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET WebP::sharpyuv APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET WebP::sharpyuv APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_LIB_DIRS_RELEASE}>)
        set_property(TARGET WebP::sharpyuv APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET WebP::sharpyuv APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_sharpyuv_COMPILE_OPTIONS_RELEASE}>)


    ########## COMPONENT WebP::webpdecoder #############

        set(libwebp_WebP_webpdecoder_FRAMEWORKS_FOUND_RELEASE "")
        conan_find_apple_frameworks(libwebp_WebP_webpdecoder_FRAMEWORKS_FOUND_RELEASE "${libwebp_WebP_webpdecoder_FRAMEWORKS_RELEASE}" "${libwebp_WebP_webpdecoder_FRAMEWORK_DIRS_RELEASE}")

        set(libwebp_WebP_webpdecoder_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET libwebp_WebP_webpdecoder_DEPS_TARGET)
            add_library(libwebp_WebP_webpdecoder_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET libwebp_WebP_webpdecoder_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_FRAMEWORKS_FOUND_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_SYSTEM_LIBS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_DEPENDENCIES_RELEASE}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'libwebp_WebP_webpdecoder_DEPS_TARGET' to all of them
        conan_package_library_targets("${libwebp_WebP_webpdecoder_LIBS_RELEASE}"
                              "${libwebp_WebP_webpdecoder_LIB_DIRS_RELEASE}"
                              "${libwebp_WebP_webpdecoder_BIN_DIRS_RELEASE}" # package_bindir
                              "${libwebp_WebP_webpdecoder_LIBRARY_TYPE_RELEASE}"
                              "${libwebp_WebP_webpdecoder_IS_HOST_WINDOWS_RELEASE}"
                              libwebp_WebP_webpdecoder_DEPS_TARGET
                              libwebp_WebP_webpdecoder_LIBRARIES_TARGETS
                              "_RELEASE"
                              "libwebp_WebP_webpdecoder"
                              "${libwebp_WebP_webpdecoder_NO_SONAME_MODE_RELEASE}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET WebP::webpdecoder
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_OBJECTS_RELEASE}>
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_LIBRARIES_TARGETS}>
                     )

        if("${libwebp_WebP_webpdecoder_LIBS_RELEASE}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET WebP::webpdecoder
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         libwebp_WebP_webpdecoder_DEPS_TARGET)
        endif()

        set_property(TARGET WebP::webpdecoder APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_LINKER_FLAGS_RELEASE}>)
        set_property(TARGET WebP::webpdecoder APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_INCLUDE_DIRS_RELEASE}>)
        set_property(TARGET WebP::webpdecoder APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_LIB_DIRS_RELEASE}>)
        set_property(TARGET WebP::webpdecoder APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_COMPILE_DEFINITIONS_RELEASE}>)
        set_property(TARGET WebP::webpdecoder APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Release>:${libwebp_WebP_webpdecoder_COMPILE_OPTIONS_RELEASE}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET libwebp::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES WebP::libwebpmux)
    set_property(TARGET libwebp::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES WebP::webpdemux)
    set_property(TARGET libwebp::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES WebP::webp)
    set_property(TARGET libwebp::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES WebP::sharpyuv)
    set_property(TARGET libwebp::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES WebP::webpdecoder)

########## For the modules (FindXXX)
set(libwebp_LIBRARIES_RELEASE libwebp::libwebp)

# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(lua_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(lua_FRAMEWORKS_FOUND_DEBUG "${lua_FRAMEWORKS_DEBUG}" "${lua_FRAMEWORK_DIRS_DEBUG}")

set(lua_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET lua_DEPS_TARGET)
    add_library(lua_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET lua_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${lua_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${lua_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### lua_DEPS_TARGET to all of them
conan_package_library_targets("${lua_LIBS_DEBUG}"    # libraries
                              "${lua_LIB_DIRS_DEBUG}" # package_libdir
                              "${lua_BIN_DIRS_DEBUG}" # package_bindir
                              "${lua_LIBRARY_TYPE_DEBUG}"
                              "${lua_IS_HOST_WINDOWS_DEBUG}"
                              lua_DEPS_TARGET
                              lua_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "lua"    # package_name
                              "${lua_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${lua_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## GLOBAL TARGET PROPERTIES Debug ########################################
    set_property(TARGET lua::lua
                 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 $<$<CONFIG:Debug>:${lua_OBJECTS_DEBUG}>
                 $<$<CONFIG:Debug>:${lua_LIBRARIES_TARGETS}>
                 )

    if("${lua_LIBS_DEBUG}" STREQUAL "")
        # If the package is not declaring any "cpp_info.libs" the package deps, system libs,
        # frameworks etc are not linked to the imported targets and we need to do it to the
        # global target
        set_property(TARGET lua::lua
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     lua_DEPS_TARGET)
    endif()

    set_property(TARGET lua::lua
                 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                 $<$<CONFIG:Debug>:${lua_LINKER_FLAGS_DEBUG}>)
    set_property(TARGET lua::lua
                 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                 $<$<CONFIG:Debug>:${lua_INCLUDE_DIRS_DEBUG}>)
    # Necessary to find LINK shared libraries in Linux
    set_property(TARGET lua::lua
                 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                 $<$<CONFIG:Debug>:${lua_LIB_DIRS_DEBUG}>)
    set_property(TARGET lua::lua
                 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                 $<$<CONFIG:Debug>:${lua_COMPILE_DEFINITIONS_DEBUG}>)
    set_property(TARGET lua::lua
                 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                 $<$<CONFIG:Debug>:${lua_COMPILE_OPTIONS_DEBUG}>)

########## For the modules (FindXXX)
set(lua_LIBRARIES_DEBUG lua::lua)

########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

list(APPEND sdl_ttf_COMPONENT_NAMES SDL2_ttf::SDL2_ttf-static)
list(REMOVE_DUPLICATES sdl_ttf_COMPONENT_NAMES)
if(DEFINED sdl_ttf_FIND_DEPENDENCY_NAMES)
  list(APPEND sdl_ttf_FIND_DEPENDENCY_NAMES freetype SDL2)
  list(REMOVE_DUPLICATES sdl_ttf_FIND_DEPENDENCY_NAMES)
else()
  set(sdl_ttf_FIND_DEPENDENCY_NAMES freetype SDL2)
endif()
set(freetype_FIND_MODE "NO_MODULE")
set(SDL2_FIND_MODE "NO_MODULE")

########### VARIABLES #######################################################################
#############################################################################################
set(sdl_ttf_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/sdl_t82b0c6147a039/p")
set(sdl_ttf_BUILD_MODULES_PATHS_DEBUG )


set(sdl_ttf_INCLUDE_DIRS_DEBUG "${sdl_ttf_PACKAGE_FOLDER_DEBUG}/include"
			"${sdl_ttf_PACKAGE_FOLDER_DEBUG}/include/SDL2")
set(sdl_ttf_RES_DIRS_DEBUG )
set(sdl_ttf_DEFINITIONS_DEBUG )
set(sdl_ttf_SHARED_LINK_FLAGS_DEBUG )
set(sdl_ttf_EXE_LINK_FLAGS_DEBUG )
set(sdl_ttf_OBJECTS_DEBUG )
set(sdl_ttf_COMPILE_DEFINITIONS_DEBUG )
set(sdl_ttf_COMPILE_OPTIONS_C_DEBUG )
set(sdl_ttf_COMPILE_OPTIONS_CXX_DEBUG )
set(sdl_ttf_LIB_DIRS_DEBUG "${sdl_ttf_PACKAGE_FOLDER_DEBUG}/lib")
set(sdl_ttf_BIN_DIRS_DEBUG )
set(sdl_ttf_LIBRARY_TYPE_DEBUG STATIC)
set(sdl_ttf_IS_HOST_WINDOWS_DEBUG 0)
set(sdl_ttf_LIBS_DEBUG SDL2_ttf)
set(sdl_ttf_SYSTEM_LIBS_DEBUG )
set(sdl_ttf_FRAMEWORK_DIRS_DEBUG )
set(sdl_ttf_FRAMEWORKS_DEBUG )
set(sdl_ttf_BUILD_DIRS_DEBUG )
set(sdl_ttf_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(sdl_ttf_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${sdl_ttf_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${sdl_ttf_COMPILE_OPTIONS_C_DEBUG}>")
set(sdl_ttf_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${sdl_ttf_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${sdl_ttf_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${sdl_ttf_EXE_LINK_FLAGS_DEBUG}>")


set(sdl_ttf_COMPONENTS_DEBUG SDL2_ttf::SDL2_ttf-static)
########### COMPONENT SDL2_ttf::SDL2_ttf-static VARIABLES ############################################

set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_INCLUDE_DIRS_DEBUG "${sdl_ttf_PACKAGE_FOLDER_DEBUG}/include"
			"${sdl_ttf_PACKAGE_FOLDER_DEBUG}/include/SDL2")
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIB_DIRS_DEBUG "${sdl_ttf_PACKAGE_FOLDER_DEBUG}/lib")
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_BIN_DIRS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBRARY_TYPE_DEBUG STATIC)
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_IS_HOST_WINDOWS_DEBUG 0)
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_RES_DIRS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEFINITIONS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_OBJECTS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_DEFINITIONS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_OPTIONS_C_DEBUG "")
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_OPTIONS_CXX_DEBUG "")
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_LIBS_DEBUG SDL2_ttf)
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_SYSTEM_LIBS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORK_DIRS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_FRAMEWORKS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_DEPENDENCIES_DEBUG Freetype::Freetype SDL2::SDL2)
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_SHARED_LINK_FLAGS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_EXE_LINK_FLAGS_DEBUG )
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_EXE_LINK_FLAGS_DEBUG}>
)
set(sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${sdl_ttf_SDL2_ttf_SDL2_ttf-static_COMPILE_OPTIONS_C_DEBUG}>")
########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(sdl_mixer_COMPONENT_NAMES "")
if(DEFINED sdl_mixer_FIND_DEPENDENCY_NAMES)
  list(APPEND sdl_mixer_FIND_DEPENDENCY_NAMES SDL2 flac mpg123 opusfile libmodplug Ogg)
  list(REMOVE_DUPLICATES sdl_mixer_FIND_DEPENDENCY_NAMES)
else()
  set(sdl_mixer_FIND_DEPENDENCY_NAMES SDL2 flac mpg123 opusfile libmodplug Ogg)
endif()
set(SDL2_FIND_MODE "NO_MODULE")
set(flac_FIND_MODE "NO_MODULE")
set(mpg123_FIND_MODE "NO_MODULE")
set(opusfile_FIND_MODE "NO_MODULE")
set(libmodplug_FIND_MODE "NO_MODULE")
set(Ogg_FIND_MODE "NO_MODULE")

########### VARIABLES #######################################################################
#############################################################################################
set(sdl_mixer_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/sdl_m476935ca42844/p")
set(sdl_mixer_BUILD_MODULES_PATHS_DEBUG )


set(sdl_mixer_INCLUDE_DIRS_DEBUG "${sdl_mixer_PACKAGE_FOLDER_DEBUG}/include"
			"${sdl_mixer_PACKAGE_FOLDER_DEBUG}/include/SDL2")
set(sdl_mixer_RES_DIRS_DEBUG )
set(sdl_mixer_DEFINITIONS_DEBUG )
set(sdl_mixer_SHARED_LINK_FLAGS_DEBUG )
set(sdl_mixer_EXE_LINK_FLAGS_DEBUG )
set(sdl_mixer_OBJECTS_DEBUG )
set(sdl_mixer_COMPILE_DEFINITIONS_DEBUG )
set(sdl_mixer_COMPILE_OPTIONS_C_DEBUG )
set(sdl_mixer_COMPILE_OPTIONS_CXX_DEBUG )
set(sdl_mixer_LIB_DIRS_DEBUG "${sdl_mixer_PACKAGE_FOLDER_DEBUG}/lib")
set(sdl_mixer_BIN_DIRS_DEBUG )
set(sdl_mixer_LIBRARY_TYPE_DEBUG STATIC)
set(sdl_mixer_IS_HOST_WINDOWS_DEBUG 0)
set(sdl_mixer_LIBS_DEBUG SDL2_mixer)
set(sdl_mixer_SYSTEM_LIBS_DEBUG )
set(sdl_mixer_FRAMEWORK_DIRS_DEBUG )
set(sdl_mixer_FRAMEWORKS_DEBUG AudioToolbox CoreServices CoreGraphics CoreFoundation AppKit AudioUnit)
set(sdl_mixer_BUILD_DIRS_DEBUG )
set(sdl_mixer_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(sdl_mixer_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${sdl_mixer_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${sdl_mixer_COMPILE_OPTIONS_C_DEBUG}>")
set(sdl_mixer_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${sdl_mixer_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${sdl_mixer_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${sdl_mixer_EXE_LINK_FLAGS_DEBUG}>")


set(sdl_mixer_COMPONENTS_DEBUG )
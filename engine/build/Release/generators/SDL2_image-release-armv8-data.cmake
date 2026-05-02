########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

list(APPEND sdl_image_COMPONENT_NAMES SDL2_image::SDL2_image)
list(REMOVE_DUPLICATES sdl_image_COMPONENT_NAMES)
if(DEFINED sdl_image_FIND_DEPENDENCY_NAMES)
  list(APPEND sdl_image_FIND_DEPENDENCY_NAMES TIFF JPEG WebP PNG SDL2)
  list(REMOVE_DUPLICATES sdl_image_FIND_DEPENDENCY_NAMES)
else()
  set(sdl_image_FIND_DEPENDENCY_NAMES TIFF JPEG WebP PNG SDL2)
endif()
set(TIFF_FIND_MODE "NO_MODULE")
set(JPEG_FIND_MODE "NO_MODULE")
set(WebP_FIND_MODE "NO_MODULE")
set(PNG_FIND_MODE "NO_MODULE")
set(SDL2_FIND_MODE "NO_MODULE")

########### VARIABLES #######################################################################
#############################################################################################
set(sdl_image_PACKAGE_FOLDER_RELEASE "/Users/ayx106047/.conan2/p/b/sdl_i22d0fae4e874c/p")
set(sdl_image_BUILD_MODULES_PATHS_RELEASE )


set(sdl_image_INCLUDE_DIRS_RELEASE "${sdl_image_PACKAGE_FOLDER_RELEASE}/include"
			"${sdl_image_PACKAGE_FOLDER_RELEASE}/include/SDL2")
set(sdl_image_RES_DIRS_RELEASE )
set(sdl_image_DEFINITIONS_RELEASE )
set(sdl_image_SHARED_LINK_FLAGS_RELEASE )
set(sdl_image_EXE_LINK_FLAGS_RELEASE )
set(sdl_image_OBJECTS_RELEASE )
set(sdl_image_COMPILE_DEFINITIONS_RELEASE )
set(sdl_image_COMPILE_OPTIONS_C_RELEASE )
set(sdl_image_COMPILE_OPTIONS_CXX_RELEASE )
set(sdl_image_LIB_DIRS_RELEASE "${sdl_image_PACKAGE_FOLDER_RELEASE}/lib")
set(sdl_image_BIN_DIRS_RELEASE )
set(sdl_image_LIBRARY_TYPE_RELEASE STATIC)
set(sdl_image_IS_HOST_WINDOWS_RELEASE 0)
set(sdl_image_LIBS_RELEASE SDL2_image)
set(sdl_image_SYSTEM_LIBS_RELEASE )
set(sdl_image_FRAMEWORK_DIRS_RELEASE )
set(sdl_image_FRAMEWORKS_RELEASE )
set(sdl_image_BUILD_DIRS_RELEASE )
set(sdl_image_NO_SONAME_MODE_RELEASE FALSE)


# COMPOUND VARIABLES
set(sdl_image_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${sdl_image_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${sdl_image_COMPILE_OPTIONS_C_RELEASE}>")
set(sdl_image_LINKER_FLAGS_RELEASE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${sdl_image_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${sdl_image_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${sdl_image_EXE_LINK_FLAGS_RELEASE}>")


set(sdl_image_COMPONENTS_RELEASE SDL2_image::SDL2_image)
########### COMPONENT SDL2_image::SDL2_image VARIABLES ############################################

set(sdl_image_SDL2_image_SDL2_image_INCLUDE_DIRS_RELEASE "${sdl_image_PACKAGE_FOLDER_RELEASE}/include"
			"${sdl_image_PACKAGE_FOLDER_RELEASE}/include/SDL2")
set(sdl_image_SDL2_image_SDL2_image_LIB_DIRS_RELEASE "${sdl_image_PACKAGE_FOLDER_RELEASE}/lib")
set(sdl_image_SDL2_image_SDL2_image_BIN_DIRS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_LIBRARY_TYPE_RELEASE STATIC)
set(sdl_image_SDL2_image_SDL2_image_IS_HOST_WINDOWS_RELEASE 0)
set(sdl_image_SDL2_image_SDL2_image_RES_DIRS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_DEFINITIONS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_OBJECTS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_COMPILE_DEFINITIONS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_C_RELEASE "")
set(sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_CXX_RELEASE "")
set(sdl_image_SDL2_image_SDL2_image_LIBS_RELEASE SDL2_image)
set(sdl_image_SDL2_image_SDL2_image_SYSTEM_LIBS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_FRAMEWORK_DIRS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_FRAMEWORKS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_DEPENDENCIES_RELEASE SDL2::SDL2main TIFF::TIFF JPEG::JPEG PNG::PNG libwebp::libwebp)
set(sdl_image_SDL2_image_SDL2_image_SHARED_LINK_FLAGS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_EXE_LINK_FLAGS_RELEASE )
set(sdl_image_SDL2_image_SDL2_image_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(sdl_image_SDL2_image_SDL2_image_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${sdl_image_SDL2_image_SDL2_image_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${sdl_image_SDL2_image_SDL2_image_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${sdl_image_SDL2_image_SDL2_image_EXE_LINK_FLAGS_RELEASE}>
)
set(sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${sdl_image_SDL2_image_SDL2_image_COMPILE_OPTIONS_C_RELEASE}>")
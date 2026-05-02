########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(libtiff_COMPONENT_NAMES "")
if(DEFINED libtiff_FIND_DEPENDENCY_NAMES)
  list(APPEND libtiff_FIND_DEPENDENCY_NAMES LibLZMA JPEG ZLIB)
  list(REMOVE_DUPLICATES libtiff_FIND_DEPENDENCY_NAMES)
else()
  set(libtiff_FIND_DEPENDENCY_NAMES LibLZMA JPEG ZLIB)
endif()
set(LibLZMA_FIND_MODE "MODULE")
set(JPEG_FIND_MODE "MODULE")
set(ZLIB_FIND_MODE "MODULE")

########### VARIABLES #######################################################################
#############################################################################################
set(libtiff_PACKAGE_FOLDER_RELEASE "/Users/ayx106047/.conan2/p/libtica5dae7248a26/p")
set(libtiff_BUILD_MODULES_PATHS_RELEASE )


set(libtiff_INCLUDE_DIRS_RELEASE )
set(libtiff_RES_DIRS_RELEASE )
set(libtiff_DEFINITIONS_RELEASE )
set(libtiff_SHARED_LINK_FLAGS_RELEASE )
set(libtiff_EXE_LINK_FLAGS_RELEASE )
set(libtiff_OBJECTS_RELEASE )
set(libtiff_COMPILE_DEFINITIONS_RELEASE )
set(libtiff_COMPILE_OPTIONS_C_RELEASE )
set(libtiff_COMPILE_OPTIONS_CXX_RELEASE )
set(libtiff_LIB_DIRS_RELEASE "${libtiff_PACKAGE_FOLDER_RELEASE}/lib")
set(libtiff_BIN_DIRS_RELEASE )
set(libtiff_LIBRARY_TYPE_RELEASE STATIC)
set(libtiff_IS_HOST_WINDOWS_RELEASE 0)
set(libtiff_LIBS_RELEASE tiffxx tiff)
set(libtiff_SYSTEM_LIBS_RELEASE )
set(libtiff_FRAMEWORK_DIRS_RELEASE )
set(libtiff_FRAMEWORKS_RELEASE )
set(libtiff_BUILD_DIRS_RELEASE )
set(libtiff_NO_SONAME_MODE_RELEASE FALSE)


# COMPOUND VARIABLES
set(libtiff_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libtiff_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libtiff_COMPILE_OPTIONS_C_RELEASE}>")
set(libtiff_LINKER_FLAGS_RELEASE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libtiff_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libtiff_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libtiff_EXE_LINK_FLAGS_RELEASE}>")


set(libtiff_COMPONENTS_RELEASE )
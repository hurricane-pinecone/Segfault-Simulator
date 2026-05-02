########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(xz_utils_COMPONENT_NAMES "")
if(DEFINED xz_utils_FIND_DEPENDENCY_NAMES)
  list(APPEND xz_utils_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES xz_utils_FIND_DEPENDENCY_NAMES)
else()
  set(xz_utils_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(xz_utils_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/xz_utc979f531b723f/p")
set(xz_utils_BUILD_MODULES_PATHS_DEBUG "${xz_utils_PACKAGE_FOLDER_DEBUG}/lib/cmake/conan-official-xz_utils-variables.cmake")


set(xz_utils_INCLUDE_DIRS_DEBUG )
set(xz_utils_RES_DIRS_DEBUG )
set(xz_utils_DEFINITIONS_DEBUG "-DLZMA_API_STATIC")
set(xz_utils_SHARED_LINK_FLAGS_DEBUG )
set(xz_utils_EXE_LINK_FLAGS_DEBUG )
set(xz_utils_OBJECTS_DEBUG )
set(xz_utils_COMPILE_DEFINITIONS_DEBUG "LZMA_API_STATIC")
set(xz_utils_COMPILE_OPTIONS_C_DEBUG )
set(xz_utils_COMPILE_OPTIONS_CXX_DEBUG )
set(xz_utils_LIB_DIRS_DEBUG "${xz_utils_PACKAGE_FOLDER_DEBUG}/lib")
set(xz_utils_BIN_DIRS_DEBUG )
set(xz_utils_LIBRARY_TYPE_DEBUG STATIC)
set(xz_utils_IS_HOST_WINDOWS_DEBUG 0)
set(xz_utils_LIBS_DEBUG lzma)
set(xz_utils_SYSTEM_LIBS_DEBUG )
set(xz_utils_FRAMEWORK_DIRS_DEBUG )
set(xz_utils_FRAMEWORKS_DEBUG )
set(xz_utils_BUILD_DIRS_DEBUG )
set(xz_utils_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(xz_utils_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${xz_utils_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${xz_utils_COMPILE_OPTIONS_C_DEBUG}>")
set(xz_utils_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${xz_utils_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${xz_utils_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${xz_utils_EXE_LINK_FLAGS_DEBUG}>")


set(xz_utils_COMPONENTS_DEBUG )
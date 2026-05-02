########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(libmodplug_COMPONENT_NAMES "")
if(DEFINED libmodplug_FIND_DEPENDENCY_NAMES)
  list(APPEND libmodplug_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES libmodplug_FIND_DEPENDENCY_NAMES)
else()
  set(libmodplug_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(libmodplug_PACKAGE_FOLDER_RELEASE "/Users/ayx106047/.conan2/p/b/libmofa04556b8dfb8/p")
set(libmodplug_BUILD_MODULES_PATHS_RELEASE )


set(libmodplug_INCLUDE_DIRS_RELEASE )
set(libmodplug_RES_DIRS_RELEASE )
set(libmodplug_DEFINITIONS_RELEASE "-DMODPLUG_STATIC")
set(libmodplug_SHARED_LINK_FLAGS_RELEASE )
set(libmodplug_EXE_LINK_FLAGS_RELEASE )
set(libmodplug_OBJECTS_RELEASE )
set(libmodplug_COMPILE_DEFINITIONS_RELEASE "MODPLUG_STATIC")
set(libmodplug_COMPILE_OPTIONS_C_RELEASE )
set(libmodplug_COMPILE_OPTIONS_CXX_RELEASE )
set(libmodplug_LIB_DIRS_RELEASE "${libmodplug_PACKAGE_FOLDER_RELEASE}/lib")
set(libmodplug_BIN_DIRS_RELEASE )
set(libmodplug_LIBRARY_TYPE_RELEASE STATIC)
set(libmodplug_IS_HOST_WINDOWS_RELEASE 0)
set(libmodplug_LIBS_RELEASE modplug)
set(libmodplug_SYSTEM_LIBS_RELEASE c++)
set(libmodplug_FRAMEWORK_DIRS_RELEASE )
set(libmodplug_FRAMEWORKS_RELEASE )
set(libmodplug_BUILD_DIRS_RELEASE )
set(libmodplug_NO_SONAME_MODE_RELEASE FALSE)


# COMPOUND VARIABLES
set(libmodplug_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libmodplug_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libmodplug_COMPILE_OPTIONS_C_RELEASE}>")
set(libmodplug_LINKER_FLAGS_RELEASE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libmodplug_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libmodplug_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libmodplug_EXE_LINK_FLAGS_RELEASE}>")


set(libmodplug_COMPONENTS_RELEASE )
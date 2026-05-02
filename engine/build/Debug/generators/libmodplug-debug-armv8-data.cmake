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
set(libmodplug_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/libmo49bcc41d8e0bb/p")
set(libmodplug_BUILD_MODULES_PATHS_DEBUG )


set(libmodplug_INCLUDE_DIRS_DEBUG )
set(libmodplug_RES_DIRS_DEBUG )
set(libmodplug_DEFINITIONS_DEBUG "-DMODPLUG_STATIC")
set(libmodplug_SHARED_LINK_FLAGS_DEBUG )
set(libmodplug_EXE_LINK_FLAGS_DEBUG )
set(libmodplug_OBJECTS_DEBUG )
set(libmodplug_COMPILE_DEFINITIONS_DEBUG "MODPLUG_STATIC")
set(libmodplug_COMPILE_OPTIONS_C_DEBUG )
set(libmodplug_COMPILE_OPTIONS_CXX_DEBUG )
set(libmodplug_LIB_DIRS_DEBUG "${libmodplug_PACKAGE_FOLDER_DEBUG}/lib")
set(libmodplug_BIN_DIRS_DEBUG )
set(libmodplug_LIBRARY_TYPE_DEBUG STATIC)
set(libmodplug_IS_HOST_WINDOWS_DEBUG 0)
set(libmodplug_LIBS_DEBUG modplug)
set(libmodplug_SYSTEM_LIBS_DEBUG c++)
set(libmodplug_FRAMEWORK_DIRS_DEBUG )
set(libmodplug_FRAMEWORKS_DEBUG )
set(libmodplug_BUILD_DIRS_DEBUG )
set(libmodplug_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(libmodplug_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libmodplug_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libmodplug_COMPILE_OPTIONS_C_DEBUG}>")
set(libmodplug_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libmodplug_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libmodplug_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libmodplug_EXE_LINK_FLAGS_DEBUG}>")


set(libmodplug_COMPONENTS_DEBUG )
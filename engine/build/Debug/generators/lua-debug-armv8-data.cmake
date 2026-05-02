########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(lua_COMPONENT_NAMES "")
if(DEFINED lua_FIND_DEPENDENCY_NAMES)
  list(APPEND lua_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES lua_FIND_DEPENDENCY_NAMES)
else()
  set(lua_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(lua_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/lua55fd76b0a1f13/p")
set(lua_BUILD_MODULES_PATHS_DEBUG )


set(lua_INCLUDE_DIRS_DEBUG "${lua_PACKAGE_FOLDER_DEBUG}/include")
set(lua_RES_DIRS_DEBUG )
set(lua_DEFINITIONS_DEBUG "-DLUA_USE_DLOPEN"
			"-DLUA_USE_POSIX")
set(lua_SHARED_LINK_FLAGS_DEBUG )
set(lua_EXE_LINK_FLAGS_DEBUG )
set(lua_OBJECTS_DEBUG )
set(lua_COMPILE_DEFINITIONS_DEBUG "LUA_USE_DLOPEN"
			"LUA_USE_POSIX")
set(lua_COMPILE_OPTIONS_C_DEBUG )
set(lua_COMPILE_OPTIONS_CXX_DEBUG )
set(lua_LIB_DIRS_DEBUG "${lua_PACKAGE_FOLDER_DEBUG}/lib")
set(lua_BIN_DIRS_DEBUG )
set(lua_LIBRARY_TYPE_DEBUG STATIC)
set(lua_IS_HOST_WINDOWS_DEBUG 0)
set(lua_LIBS_DEBUG lua)
set(lua_SYSTEM_LIBS_DEBUG )
set(lua_FRAMEWORK_DIRS_DEBUG )
set(lua_FRAMEWORKS_DEBUG )
set(lua_BUILD_DIRS_DEBUG )
set(lua_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(lua_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${lua_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${lua_COMPILE_OPTIONS_C_DEBUG}>")
set(lua_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${lua_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${lua_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${lua_EXE_LINK_FLAGS_DEBUG}>")


set(lua_COMPONENTS_DEBUG )
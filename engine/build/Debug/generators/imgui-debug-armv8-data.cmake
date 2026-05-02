########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(imgui_COMPONENT_NAMES "")
if(DEFINED imgui_FIND_DEPENDENCY_NAMES)
  list(APPEND imgui_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES imgui_FIND_DEPENDENCY_NAMES)
else()
  set(imgui_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(imgui_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/imguif0b0c93582b8a/p")
set(imgui_BUILD_MODULES_PATHS_DEBUG )


set(imgui_INCLUDE_DIRS_DEBUG "${imgui_PACKAGE_FOLDER_DEBUG}/include")
set(imgui_RES_DIRS_DEBUG )
set(imgui_DEFINITIONS_DEBUG )
set(imgui_SHARED_LINK_FLAGS_DEBUG )
set(imgui_EXE_LINK_FLAGS_DEBUG )
set(imgui_OBJECTS_DEBUG )
set(imgui_COMPILE_DEFINITIONS_DEBUG )
set(imgui_COMPILE_OPTIONS_C_DEBUG )
set(imgui_COMPILE_OPTIONS_CXX_DEBUG )
set(imgui_LIB_DIRS_DEBUG "${imgui_PACKAGE_FOLDER_DEBUG}/lib")
set(imgui_BIN_DIRS_DEBUG )
set(imgui_LIBRARY_TYPE_DEBUG STATIC)
set(imgui_IS_HOST_WINDOWS_DEBUG 0)
set(imgui_LIBS_DEBUG imgui)
set(imgui_SYSTEM_LIBS_DEBUG )
set(imgui_FRAMEWORK_DIRS_DEBUG )
set(imgui_FRAMEWORKS_DEBUG )
set(imgui_BUILD_DIRS_DEBUG )
set(imgui_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(imgui_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${imgui_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${imgui_COMPILE_OPTIONS_C_DEBUG}>")
set(imgui_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${imgui_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${imgui_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${imgui_EXE_LINK_FLAGS_DEBUG}>")


set(imgui_COMPONENTS_DEBUG )
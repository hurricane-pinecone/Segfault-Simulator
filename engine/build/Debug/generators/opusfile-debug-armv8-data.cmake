########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

list(APPEND opusfile_COMPONENT_NAMES opusfile::libopusfile opusfile::opusurl)
list(REMOVE_DUPLICATES opusfile_COMPONENT_NAMES)
if(DEFINED opusfile_FIND_DEPENDENCY_NAMES)
  list(APPEND opusfile_FIND_DEPENDENCY_NAMES Opus OpenSSL Ogg)
  list(REMOVE_DUPLICATES opusfile_FIND_DEPENDENCY_NAMES)
else()
  set(opusfile_FIND_DEPENDENCY_NAMES Opus OpenSSL Ogg)
endif()
set(Opus_FIND_MODE "NO_MODULE")
set(OpenSSL_FIND_MODE "NO_MODULE")
set(Ogg_FIND_MODE "NO_MODULE")

########### VARIABLES #######################################################################
#############################################################################################
set(opusfile_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/opusfbaec9d1fea938/p")
set(opusfile_BUILD_MODULES_PATHS_DEBUG )


set(opusfile_INCLUDE_DIRS_DEBUG )
set(opusfile_RES_DIRS_DEBUG )
set(opusfile_DEFINITIONS_DEBUG )
set(opusfile_SHARED_LINK_FLAGS_DEBUG )
set(opusfile_EXE_LINK_FLAGS_DEBUG )
set(opusfile_OBJECTS_DEBUG )
set(opusfile_COMPILE_DEFINITIONS_DEBUG )
set(opusfile_COMPILE_OPTIONS_C_DEBUG )
set(opusfile_COMPILE_OPTIONS_CXX_DEBUG )
set(opusfile_LIB_DIRS_DEBUG "${opusfile_PACKAGE_FOLDER_DEBUG}/lib")
set(opusfile_BIN_DIRS_DEBUG )
set(opusfile_LIBRARY_TYPE_DEBUG STATIC)
set(opusfile_IS_HOST_WINDOWS_DEBUG 0)
set(opusfile_LIBS_DEBUG opusurl opusfile)
set(opusfile_SYSTEM_LIBS_DEBUG )
set(opusfile_FRAMEWORK_DIRS_DEBUG )
set(opusfile_FRAMEWORKS_DEBUG )
set(opusfile_BUILD_DIRS_DEBUG )
set(opusfile_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(opusfile_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${opusfile_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${opusfile_COMPILE_OPTIONS_C_DEBUG}>")
set(opusfile_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${opusfile_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${opusfile_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${opusfile_EXE_LINK_FLAGS_DEBUG}>")


set(opusfile_COMPONENTS_DEBUG opusfile::libopusfile opusfile::opusurl)
########### COMPONENT opusfile::opusurl VARIABLES ############################################

set(opusfile_opusfile_opusurl_INCLUDE_DIRS_DEBUG )
set(opusfile_opusfile_opusurl_LIB_DIRS_DEBUG "${opusfile_PACKAGE_FOLDER_DEBUG}/lib")
set(opusfile_opusfile_opusurl_BIN_DIRS_DEBUG )
set(opusfile_opusfile_opusurl_LIBRARY_TYPE_DEBUG STATIC)
set(opusfile_opusfile_opusurl_IS_HOST_WINDOWS_DEBUG 0)
set(opusfile_opusfile_opusurl_RES_DIRS_DEBUG )
set(opusfile_opusfile_opusurl_DEFINITIONS_DEBUG )
set(opusfile_opusfile_opusurl_OBJECTS_DEBUG )
set(opusfile_opusfile_opusurl_COMPILE_DEFINITIONS_DEBUG )
set(opusfile_opusfile_opusurl_COMPILE_OPTIONS_C_DEBUG "")
set(opusfile_opusfile_opusurl_COMPILE_OPTIONS_CXX_DEBUG "")
set(opusfile_opusfile_opusurl_LIBS_DEBUG opusurl)
set(opusfile_opusfile_opusurl_SYSTEM_LIBS_DEBUG )
set(opusfile_opusfile_opusurl_FRAMEWORK_DIRS_DEBUG )
set(opusfile_opusfile_opusurl_FRAMEWORKS_DEBUG )
set(opusfile_opusfile_opusurl_DEPENDENCIES_DEBUG opusfile::libopusfile openssl::openssl)
set(opusfile_opusfile_opusurl_SHARED_LINK_FLAGS_DEBUG )
set(opusfile_opusfile_opusurl_EXE_LINK_FLAGS_DEBUG )
set(opusfile_opusfile_opusurl_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(opusfile_opusfile_opusurl_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${opusfile_opusfile_opusurl_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${opusfile_opusfile_opusurl_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${opusfile_opusfile_opusurl_EXE_LINK_FLAGS_DEBUG}>
)
set(opusfile_opusfile_opusurl_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${opusfile_opusfile_opusurl_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${opusfile_opusfile_opusurl_COMPILE_OPTIONS_C_DEBUG}>")
########### COMPONENT opusfile::libopusfile VARIABLES ############################################

set(opusfile_opusfile_libopusfile_INCLUDE_DIRS_DEBUG )
set(opusfile_opusfile_libopusfile_LIB_DIRS_DEBUG "${opusfile_PACKAGE_FOLDER_DEBUG}/lib")
set(opusfile_opusfile_libopusfile_BIN_DIRS_DEBUG )
set(opusfile_opusfile_libopusfile_LIBRARY_TYPE_DEBUG STATIC)
set(opusfile_opusfile_libopusfile_IS_HOST_WINDOWS_DEBUG 0)
set(opusfile_opusfile_libopusfile_RES_DIRS_DEBUG )
set(opusfile_opusfile_libopusfile_DEFINITIONS_DEBUG )
set(opusfile_opusfile_libopusfile_OBJECTS_DEBUG )
set(opusfile_opusfile_libopusfile_COMPILE_DEFINITIONS_DEBUG )
set(opusfile_opusfile_libopusfile_COMPILE_OPTIONS_C_DEBUG "")
set(opusfile_opusfile_libopusfile_COMPILE_OPTIONS_CXX_DEBUG "")
set(opusfile_opusfile_libopusfile_LIBS_DEBUG opusfile)
set(opusfile_opusfile_libopusfile_SYSTEM_LIBS_DEBUG )
set(opusfile_opusfile_libopusfile_FRAMEWORK_DIRS_DEBUG )
set(opusfile_opusfile_libopusfile_FRAMEWORKS_DEBUG )
set(opusfile_opusfile_libopusfile_DEPENDENCIES_DEBUG Ogg::ogg Opus::opus)
set(opusfile_opusfile_libopusfile_SHARED_LINK_FLAGS_DEBUG )
set(opusfile_opusfile_libopusfile_EXE_LINK_FLAGS_DEBUG )
set(opusfile_opusfile_libopusfile_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(opusfile_opusfile_libopusfile_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${opusfile_opusfile_libopusfile_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${opusfile_opusfile_libopusfile_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${opusfile_opusfile_libopusfile_EXE_LINK_FLAGS_DEBUG}>
)
set(opusfile_opusfile_libopusfile_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${opusfile_opusfile_libopusfile_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${opusfile_opusfile_libopusfile_COMPILE_OPTIONS_C_DEBUG}>")
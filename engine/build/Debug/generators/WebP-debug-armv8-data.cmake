########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

list(APPEND libwebp_COMPONENT_NAMES WebP::webpdecoder WebP::sharpyuv WebP::webp WebP::webpdemux WebP::libwebpmux)
list(REMOVE_DUPLICATES libwebp_COMPONENT_NAMES)
if(DEFINED libwebp_FIND_DEPENDENCY_NAMES)
  list(APPEND libwebp_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES libwebp_FIND_DEPENDENCY_NAMES)
else()
  set(libwebp_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(libwebp_PACKAGE_FOLDER_DEBUG "/Users/ayx106047/.conan2/p/b/libwec1bca1dcc5a3f/p")
set(libwebp_BUILD_MODULES_PATHS_DEBUG )


set(libwebp_INCLUDE_DIRS_DEBUG )
set(libwebp_RES_DIRS_DEBUG )
set(libwebp_DEFINITIONS_DEBUG )
set(libwebp_SHARED_LINK_FLAGS_DEBUG )
set(libwebp_EXE_LINK_FLAGS_DEBUG )
set(libwebp_OBJECTS_DEBUG )
set(libwebp_COMPILE_DEFINITIONS_DEBUG )
set(libwebp_COMPILE_OPTIONS_C_DEBUG )
set(libwebp_COMPILE_OPTIONS_CXX_DEBUG )
set(libwebp_LIB_DIRS_DEBUG "${libwebp_PACKAGE_FOLDER_DEBUG}/lib")
set(libwebp_BIN_DIRS_DEBUG )
set(libwebp_LIBRARY_TYPE_DEBUG STATIC)
set(libwebp_IS_HOST_WINDOWS_DEBUG 0)
set(libwebp_LIBS_DEBUG webpmux webpdemux webp sharpyuv webpdecoder)
set(libwebp_SYSTEM_LIBS_DEBUG )
set(libwebp_FRAMEWORK_DIRS_DEBUG )
set(libwebp_FRAMEWORKS_DEBUG )
set(libwebp_BUILD_DIRS_DEBUG )
set(libwebp_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(libwebp_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_COMPILE_OPTIONS_C_DEBUG}>")
set(libwebp_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_EXE_LINK_FLAGS_DEBUG}>")


set(libwebp_COMPONENTS_DEBUG WebP::webpdecoder WebP::sharpyuv WebP::webp WebP::webpdemux WebP::libwebpmux)
########### COMPONENT WebP::libwebpmux VARIABLES ############################################

set(libwebp_WebP_libwebpmux_INCLUDE_DIRS_DEBUG )
set(libwebp_WebP_libwebpmux_LIB_DIRS_DEBUG "${libwebp_PACKAGE_FOLDER_DEBUG}/lib")
set(libwebp_WebP_libwebpmux_BIN_DIRS_DEBUG )
set(libwebp_WebP_libwebpmux_LIBRARY_TYPE_DEBUG STATIC)
set(libwebp_WebP_libwebpmux_IS_HOST_WINDOWS_DEBUG 0)
set(libwebp_WebP_libwebpmux_RES_DIRS_DEBUG )
set(libwebp_WebP_libwebpmux_DEFINITIONS_DEBUG )
set(libwebp_WebP_libwebpmux_OBJECTS_DEBUG )
set(libwebp_WebP_libwebpmux_COMPILE_DEFINITIONS_DEBUG )
set(libwebp_WebP_libwebpmux_COMPILE_OPTIONS_C_DEBUG "")
set(libwebp_WebP_libwebpmux_COMPILE_OPTIONS_CXX_DEBUG "")
set(libwebp_WebP_libwebpmux_LIBS_DEBUG webpmux)
set(libwebp_WebP_libwebpmux_SYSTEM_LIBS_DEBUG )
set(libwebp_WebP_libwebpmux_FRAMEWORK_DIRS_DEBUG )
set(libwebp_WebP_libwebpmux_FRAMEWORKS_DEBUG )
set(libwebp_WebP_libwebpmux_DEPENDENCIES_DEBUG WebP::webp)
set(libwebp_WebP_libwebpmux_SHARED_LINK_FLAGS_DEBUG )
set(libwebp_WebP_libwebpmux_EXE_LINK_FLAGS_DEBUG )
set(libwebp_WebP_libwebpmux_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_libwebpmux_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_libwebpmux_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_libwebpmux_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_libwebpmux_EXE_LINK_FLAGS_DEBUG}>
)
set(libwebp_WebP_libwebpmux_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_libwebpmux_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_libwebpmux_COMPILE_OPTIONS_C_DEBUG}>")
########### COMPONENT WebP::webpdemux VARIABLES ############################################

set(libwebp_WebP_webpdemux_INCLUDE_DIRS_DEBUG )
set(libwebp_WebP_webpdemux_LIB_DIRS_DEBUG "${libwebp_PACKAGE_FOLDER_DEBUG}/lib")
set(libwebp_WebP_webpdemux_BIN_DIRS_DEBUG )
set(libwebp_WebP_webpdemux_LIBRARY_TYPE_DEBUG STATIC)
set(libwebp_WebP_webpdemux_IS_HOST_WINDOWS_DEBUG 0)
set(libwebp_WebP_webpdemux_RES_DIRS_DEBUG )
set(libwebp_WebP_webpdemux_DEFINITIONS_DEBUG )
set(libwebp_WebP_webpdemux_OBJECTS_DEBUG )
set(libwebp_WebP_webpdemux_COMPILE_DEFINITIONS_DEBUG )
set(libwebp_WebP_webpdemux_COMPILE_OPTIONS_C_DEBUG "")
set(libwebp_WebP_webpdemux_COMPILE_OPTIONS_CXX_DEBUG "")
set(libwebp_WebP_webpdemux_LIBS_DEBUG webpdemux)
set(libwebp_WebP_webpdemux_SYSTEM_LIBS_DEBUG )
set(libwebp_WebP_webpdemux_FRAMEWORK_DIRS_DEBUG )
set(libwebp_WebP_webpdemux_FRAMEWORKS_DEBUG )
set(libwebp_WebP_webpdemux_DEPENDENCIES_DEBUG WebP::webp)
set(libwebp_WebP_webpdemux_SHARED_LINK_FLAGS_DEBUG )
set(libwebp_WebP_webpdemux_EXE_LINK_FLAGS_DEBUG )
set(libwebp_WebP_webpdemux_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_webpdemux_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_webpdemux_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_webpdemux_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_webpdemux_EXE_LINK_FLAGS_DEBUG}>
)
set(libwebp_WebP_webpdemux_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_webpdemux_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_webpdemux_COMPILE_OPTIONS_C_DEBUG}>")
########### COMPONENT WebP::webp VARIABLES ############################################

set(libwebp_WebP_webp_INCLUDE_DIRS_DEBUG )
set(libwebp_WebP_webp_LIB_DIRS_DEBUG "${libwebp_PACKAGE_FOLDER_DEBUG}/lib")
set(libwebp_WebP_webp_BIN_DIRS_DEBUG )
set(libwebp_WebP_webp_LIBRARY_TYPE_DEBUG STATIC)
set(libwebp_WebP_webp_IS_HOST_WINDOWS_DEBUG 0)
set(libwebp_WebP_webp_RES_DIRS_DEBUG )
set(libwebp_WebP_webp_DEFINITIONS_DEBUG )
set(libwebp_WebP_webp_OBJECTS_DEBUG )
set(libwebp_WebP_webp_COMPILE_DEFINITIONS_DEBUG )
set(libwebp_WebP_webp_COMPILE_OPTIONS_C_DEBUG "")
set(libwebp_WebP_webp_COMPILE_OPTIONS_CXX_DEBUG "")
set(libwebp_WebP_webp_LIBS_DEBUG webp)
set(libwebp_WebP_webp_SYSTEM_LIBS_DEBUG )
set(libwebp_WebP_webp_FRAMEWORK_DIRS_DEBUG )
set(libwebp_WebP_webp_FRAMEWORKS_DEBUG )
set(libwebp_WebP_webp_DEPENDENCIES_DEBUG WebP::sharpyuv)
set(libwebp_WebP_webp_SHARED_LINK_FLAGS_DEBUG )
set(libwebp_WebP_webp_EXE_LINK_FLAGS_DEBUG )
set(libwebp_WebP_webp_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_webp_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_webp_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_webp_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_webp_EXE_LINK_FLAGS_DEBUG}>
)
set(libwebp_WebP_webp_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_webp_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_webp_COMPILE_OPTIONS_C_DEBUG}>")
########### COMPONENT WebP::sharpyuv VARIABLES ############################################

set(libwebp_WebP_sharpyuv_INCLUDE_DIRS_DEBUG )
set(libwebp_WebP_sharpyuv_LIB_DIRS_DEBUG "${libwebp_PACKAGE_FOLDER_DEBUG}/lib")
set(libwebp_WebP_sharpyuv_BIN_DIRS_DEBUG )
set(libwebp_WebP_sharpyuv_LIBRARY_TYPE_DEBUG STATIC)
set(libwebp_WebP_sharpyuv_IS_HOST_WINDOWS_DEBUG 0)
set(libwebp_WebP_sharpyuv_RES_DIRS_DEBUG )
set(libwebp_WebP_sharpyuv_DEFINITIONS_DEBUG )
set(libwebp_WebP_sharpyuv_OBJECTS_DEBUG )
set(libwebp_WebP_sharpyuv_COMPILE_DEFINITIONS_DEBUG )
set(libwebp_WebP_sharpyuv_COMPILE_OPTIONS_C_DEBUG "")
set(libwebp_WebP_sharpyuv_COMPILE_OPTIONS_CXX_DEBUG "")
set(libwebp_WebP_sharpyuv_LIBS_DEBUG sharpyuv)
set(libwebp_WebP_sharpyuv_SYSTEM_LIBS_DEBUG )
set(libwebp_WebP_sharpyuv_FRAMEWORK_DIRS_DEBUG )
set(libwebp_WebP_sharpyuv_FRAMEWORKS_DEBUG )
set(libwebp_WebP_sharpyuv_DEPENDENCIES_DEBUG )
set(libwebp_WebP_sharpyuv_SHARED_LINK_FLAGS_DEBUG )
set(libwebp_WebP_sharpyuv_EXE_LINK_FLAGS_DEBUG )
set(libwebp_WebP_sharpyuv_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_sharpyuv_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_sharpyuv_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_sharpyuv_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_sharpyuv_EXE_LINK_FLAGS_DEBUG}>
)
set(libwebp_WebP_sharpyuv_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_sharpyuv_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_sharpyuv_COMPILE_OPTIONS_C_DEBUG}>")
########### COMPONENT WebP::webpdecoder VARIABLES ############################################

set(libwebp_WebP_webpdecoder_INCLUDE_DIRS_DEBUG )
set(libwebp_WebP_webpdecoder_LIB_DIRS_DEBUG "${libwebp_PACKAGE_FOLDER_DEBUG}/lib")
set(libwebp_WebP_webpdecoder_BIN_DIRS_DEBUG )
set(libwebp_WebP_webpdecoder_LIBRARY_TYPE_DEBUG STATIC)
set(libwebp_WebP_webpdecoder_IS_HOST_WINDOWS_DEBUG 0)
set(libwebp_WebP_webpdecoder_RES_DIRS_DEBUG )
set(libwebp_WebP_webpdecoder_DEFINITIONS_DEBUG )
set(libwebp_WebP_webpdecoder_OBJECTS_DEBUG )
set(libwebp_WebP_webpdecoder_COMPILE_DEFINITIONS_DEBUG )
set(libwebp_WebP_webpdecoder_COMPILE_OPTIONS_C_DEBUG "")
set(libwebp_WebP_webpdecoder_COMPILE_OPTIONS_CXX_DEBUG "")
set(libwebp_WebP_webpdecoder_LIBS_DEBUG webpdecoder)
set(libwebp_WebP_webpdecoder_SYSTEM_LIBS_DEBUG )
set(libwebp_WebP_webpdecoder_FRAMEWORK_DIRS_DEBUG )
set(libwebp_WebP_webpdecoder_FRAMEWORKS_DEBUG )
set(libwebp_WebP_webpdecoder_DEPENDENCIES_DEBUG )
set(libwebp_WebP_webpdecoder_SHARED_LINK_FLAGS_DEBUG )
set(libwebp_WebP_webpdecoder_EXE_LINK_FLAGS_DEBUG )
set(libwebp_WebP_webpdecoder_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_webpdecoder_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_webpdecoder_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_webpdecoder_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_webpdecoder_EXE_LINK_FLAGS_DEBUG}>
)
set(libwebp_WebP_webpdecoder_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_webpdecoder_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_webpdecoder_COMPILE_OPTIONS_C_DEBUG}>")
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
set(libwebp_PACKAGE_FOLDER_RELEASE "/Users/ayx106047/.conan2/p/b/libwe414de368a2cc2/p")
set(libwebp_BUILD_MODULES_PATHS_RELEASE )


set(libwebp_INCLUDE_DIRS_RELEASE )
set(libwebp_RES_DIRS_RELEASE )
set(libwebp_DEFINITIONS_RELEASE )
set(libwebp_SHARED_LINK_FLAGS_RELEASE )
set(libwebp_EXE_LINK_FLAGS_RELEASE )
set(libwebp_OBJECTS_RELEASE )
set(libwebp_COMPILE_DEFINITIONS_RELEASE )
set(libwebp_COMPILE_OPTIONS_C_RELEASE )
set(libwebp_COMPILE_OPTIONS_CXX_RELEASE )
set(libwebp_LIB_DIRS_RELEASE "${libwebp_PACKAGE_FOLDER_RELEASE}/lib")
set(libwebp_BIN_DIRS_RELEASE )
set(libwebp_LIBRARY_TYPE_RELEASE STATIC)
set(libwebp_IS_HOST_WINDOWS_RELEASE 0)
set(libwebp_LIBS_RELEASE webpmux webpdemux webp sharpyuv webpdecoder)
set(libwebp_SYSTEM_LIBS_RELEASE )
set(libwebp_FRAMEWORK_DIRS_RELEASE )
set(libwebp_FRAMEWORKS_RELEASE )
set(libwebp_BUILD_DIRS_RELEASE )
set(libwebp_NO_SONAME_MODE_RELEASE FALSE)


# COMPOUND VARIABLES
set(libwebp_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_COMPILE_OPTIONS_C_RELEASE}>")
set(libwebp_LINKER_FLAGS_RELEASE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_SHARED_LINK_FLAGS_RELEASE}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_EXE_LINK_FLAGS_RELEASE}>")


set(libwebp_COMPONENTS_RELEASE WebP::webpdecoder WebP::sharpyuv WebP::webp WebP::webpdemux WebP::libwebpmux)
########### COMPONENT WebP::libwebpmux VARIABLES ############################################

set(libwebp_WebP_libwebpmux_INCLUDE_DIRS_RELEASE )
set(libwebp_WebP_libwebpmux_LIB_DIRS_RELEASE "${libwebp_PACKAGE_FOLDER_RELEASE}/lib")
set(libwebp_WebP_libwebpmux_BIN_DIRS_RELEASE )
set(libwebp_WebP_libwebpmux_LIBRARY_TYPE_RELEASE STATIC)
set(libwebp_WebP_libwebpmux_IS_HOST_WINDOWS_RELEASE 0)
set(libwebp_WebP_libwebpmux_RES_DIRS_RELEASE )
set(libwebp_WebP_libwebpmux_DEFINITIONS_RELEASE )
set(libwebp_WebP_libwebpmux_OBJECTS_RELEASE )
set(libwebp_WebP_libwebpmux_COMPILE_DEFINITIONS_RELEASE )
set(libwebp_WebP_libwebpmux_COMPILE_OPTIONS_C_RELEASE "")
set(libwebp_WebP_libwebpmux_COMPILE_OPTIONS_CXX_RELEASE "")
set(libwebp_WebP_libwebpmux_LIBS_RELEASE webpmux)
set(libwebp_WebP_libwebpmux_SYSTEM_LIBS_RELEASE )
set(libwebp_WebP_libwebpmux_FRAMEWORK_DIRS_RELEASE )
set(libwebp_WebP_libwebpmux_FRAMEWORKS_RELEASE )
set(libwebp_WebP_libwebpmux_DEPENDENCIES_RELEASE WebP::webp)
set(libwebp_WebP_libwebpmux_SHARED_LINK_FLAGS_RELEASE )
set(libwebp_WebP_libwebpmux_EXE_LINK_FLAGS_RELEASE )
set(libwebp_WebP_libwebpmux_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_libwebpmux_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_libwebpmux_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_libwebpmux_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_libwebpmux_EXE_LINK_FLAGS_RELEASE}>
)
set(libwebp_WebP_libwebpmux_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_libwebpmux_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_libwebpmux_COMPILE_OPTIONS_C_RELEASE}>")
########### COMPONENT WebP::webpdemux VARIABLES ############################################

set(libwebp_WebP_webpdemux_INCLUDE_DIRS_RELEASE )
set(libwebp_WebP_webpdemux_LIB_DIRS_RELEASE "${libwebp_PACKAGE_FOLDER_RELEASE}/lib")
set(libwebp_WebP_webpdemux_BIN_DIRS_RELEASE )
set(libwebp_WebP_webpdemux_LIBRARY_TYPE_RELEASE STATIC)
set(libwebp_WebP_webpdemux_IS_HOST_WINDOWS_RELEASE 0)
set(libwebp_WebP_webpdemux_RES_DIRS_RELEASE )
set(libwebp_WebP_webpdemux_DEFINITIONS_RELEASE )
set(libwebp_WebP_webpdemux_OBJECTS_RELEASE )
set(libwebp_WebP_webpdemux_COMPILE_DEFINITIONS_RELEASE )
set(libwebp_WebP_webpdemux_COMPILE_OPTIONS_C_RELEASE "")
set(libwebp_WebP_webpdemux_COMPILE_OPTIONS_CXX_RELEASE "")
set(libwebp_WebP_webpdemux_LIBS_RELEASE webpdemux)
set(libwebp_WebP_webpdemux_SYSTEM_LIBS_RELEASE )
set(libwebp_WebP_webpdemux_FRAMEWORK_DIRS_RELEASE )
set(libwebp_WebP_webpdemux_FRAMEWORKS_RELEASE )
set(libwebp_WebP_webpdemux_DEPENDENCIES_RELEASE WebP::webp)
set(libwebp_WebP_webpdemux_SHARED_LINK_FLAGS_RELEASE )
set(libwebp_WebP_webpdemux_EXE_LINK_FLAGS_RELEASE )
set(libwebp_WebP_webpdemux_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_webpdemux_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_webpdemux_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_webpdemux_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_webpdemux_EXE_LINK_FLAGS_RELEASE}>
)
set(libwebp_WebP_webpdemux_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_webpdemux_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_webpdemux_COMPILE_OPTIONS_C_RELEASE}>")
########### COMPONENT WebP::webp VARIABLES ############################################

set(libwebp_WebP_webp_INCLUDE_DIRS_RELEASE )
set(libwebp_WebP_webp_LIB_DIRS_RELEASE "${libwebp_PACKAGE_FOLDER_RELEASE}/lib")
set(libwebp_WebP_webp_BIN_DIRS_RELEASE )
set(libwebp_WebP_webp_LIBRARY_TYPE_RELEASE STATIC)
set(libwebp_WebP_webp_IS_HOST_WINDOWS_RELEASE 0)
set(libwebp_WebP_webp_RES_DIRS_RELEASE )
set(libwebp_WebP_webp_DEFINITIONS_RELEASE )
set(libwebp_WebP_webp_OBJECTS_RELEASE )
set(libwebp_WebP_webp_COMPILE_DEFINITIONS_RELEASE )
set(libwebp_WebP_webp_COMPILE_OPTIONS_C_RELEASE "")
set(libwebp_WebP_webp_COMPILE_OPTIONS_CXX_RELEASE "")
set(libwebp_WebP_webp_LIBS_RELEASE webp)
set(libwebp_WebP_webp_SYSTEM_LIBS_RELEASE )
set(libwebp_WebP_webp_FRAMEWORK_DIRS_RELEASE )
set(libwebp_WebP_webp_FRAMEWORKS_RELEASE )
set(libwebp_WebP_webp_DEPENDENCIES_RELEASE WebP::sharpyuv)
set(libwebp_WebP_webp_SHARED_LINK_FLAGS_RELEASE )
set(libwebp_WebP_webp_EXE_LINK_FLAGS_RELEASE )
set(libwebp_WebP_webp_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_webp_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_webp_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_webp_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_webp_EXE_LINK_FLAGS_RELEASE}>
)
set(libwebp_WebP_webp_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_webp_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_webp_COMPILE_OPTIONS_C_RELEASE}>")
########### COMPONENT WebP::sharpyuv VARIABLES ############################################

set(libwebp_WebP_sharpyuv_INCLUDE_DIRS_RELEASE )
set(libwebp_WebP_sharpyuv_LIB_DIRS_RELEASE "${libwebp_PACKAGE_FOLDER_RELEASE}/lib")
set(libwebp_WebP_sharpyuv_BIN_DIRS_RELEASE )
set(libwebp_WebP_sharpyuv_LIBRARY_TYPE_RELEASE STATIC)
set(libwebp_WebP_sharpyuv_IS_HOST_WINDOWS_RELEASE 0)
set(libwebp_WebP_sharpyuv_RES_DIRS_RELEASE )
set(libwebp_WebP_sharpyuv_DEFINITIONS_RELEASE )
set(libwebp_WebP_sharpyuv_OBJECTS_RELEASE )
set(libwebp_WebP_sharpyuv_COMPILE_DEFINITIONS_RELEASE )
set(libwebp_WebP_sharpyuv_COMPILE_OPTIONS_C_RELEASE "")
set(libwebp_WebP_sharpyuv_COMPILE_OPTIONS_CXX_RELEASE "")
set(libwebp_WebP_sharpyuv_LIBS_RELEASE sharpyuv)
set(libwebp_WebP_sharpyuv_SYSTEM_LIBS_RELEASE )
set(libwebp_WebP_sharpyuv_FRAMEWORK_DIRS_RELEASE )
set(libwebp_WebP_sharpyuv_FRAMEWORKS_RELEASE )
set(libwebp_WebP_sharpyuv_DEPENDENCIES_RELEASE )
set(libwebp_WebP_sharpyuv_SHARED_LINK_FLAGS_RELEASE )
set(libwebp_WebP_sharpyuv_EXE_LINK_FLAGS_RELEASE )
set(libwebp_WebP_sharpyuv_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_sharpyuv_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_sharpyuv_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_sharpyuv_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_sharpyuv_EXE_LINK_FLAGS_RELEASE}>
)
set(libwebp_WebP_sharpyuv_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_sharpyuv_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_sharpyuv_COMPILE_OPTIONS_C_RELEASE}>")
########### COMPONENT WebP::webpdecoder VARIABLES ############################################

set(libwebp_WebP_webpdecoder_INCLUDE_DIRS_RELEASE )
set(libwebp_WebP_webpdecoder_LIB_DIRS_RELEASE "${libwebp_PACKAGE_FOLDER_RELEASE}/lib")
set(libwebp_WebP_webpdecoder_BIN_DIRS_RELEASE )
set(libwebp_WebP_webpdecoder_LIBRARY_TYPE_RELEASE STATIC)
set(libwebp_WebP_webpdecoder_IS_HOST_WINDOWS_RELEASE 0)
set(libwebp_WebP_webpdecoder_RES_DIRS_RELEASE )
set(libwebp_WebP_webpdecoder_DEFINITIONS_RELEASE )
set(libwebp_WebP_webpdecoder_OBJECTS_RELEASE )
set(libwebp_WebP_webpdecoder_COMPILE_DEFINITIONS_RELEASE )
set(libwebp_WebP_webpdecoder_COMPILE_OPTIONS_C_RELEASE "")
set(libwebp_WebP_webpdecoder_COMPILE_OPTIONS_CXX_RELEASE "")
set(libwebp_WebP_webpdecoder_LIBS_RELEASE webpdecoder)
set(libwebp_WebP_webpdecoder_SYSTEM_LIBS_RELEASE )
set(libwebp_WebP_webpdecoder_FRAMEWORK_DIRS_RELEASE )
set(libwebp_WebP_webpdecoder_FRAMEWORKS_RELEASE )
set(libwebp_WebP_webpdecoder_DEPENDENCIES_RELEASE )
set(libwebp_WebP_webpdecoder_SHARED_LINK_FLAGS_RELEASE )
set(libwebp_WebP_webpdecoder_EXE_LINK_FLAGS_RELEASE )
set(libwebp_WebP_webpdecoder_NO_SONAME_MODE_RELEASE FALSE)

# COMPOUND VARIABLES
set(libwebp_WebP_webpdecoder_LINKER_FLAGS_RELEASE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${libwebp_WebP_webpdecoder_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${libwebp_WebP_webpdecoder_SHARED_LINK_FLAGS_RELEASE}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${libwebp_WebP_webpdecoder_EXE_LINK_FLAGS_RELEASE}>
)
set(libwebp_WebP_webpdecoder_COMPILE_OPTIONS_RELEASE
    "$<$<COMPILE_LANGUAGE:CXX>:${libwebp_WebP_webpdecoder_COMPILE_OPTIONS_CXX_RELEASE}>"
    "$<$<COMPILE_LANGUAGE:C>:${libwebp_WebP_webpdecoder_COMPILE_OPTIONS_C_RELEASE}>")
# Conan automatically generated toolchain file
# DO NOT EDIT MANUALLY, it will be overwritten

# Avoid including toolchain file several times (bad if appending to variables like
#   CMAKE_CXX_FLAGS. See https://github.com/android/ndk/issues/323
include_guard()
message(STATUS "Using Conan toolchain: ${CMAKE_CURRENT_LIST_FILE}")
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeToolchain' generator only works with CMake >= 3.15")
endif()

########## 'user_toolchain' block #############
# Include one or more CMake user toolchain from tools.cmake.cmaketoolchain:user_toolchain



########## 'generic_system' block #############
# Definition of system, platform and toolset





########## 'compilers' block #############



########## 'apple_system' block #############
# Define Apple architectures, sysroot, deployment target, bitcode, etc

# Set the architectures for which to build.
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
# Setting CMAKE_OSX_SYSROOT SDK, when using Xcode generator the name is enough
# but full path is necessary for others
set(CMAKE_OSX_SYSROOT macosx CACHE STRING "" FORCE)
set(BITCODE "")
set(FOBJC_ARC "")
set(VISIBILITY "")
#Check if Xcode generator is used, since that will handle these flags automagically
if(CMAKE_GENERATOR MATCHES "Xcode")
  message(DEBUG "Not setting any manual command-line buildflags, since Xcode is selected as generator.")
else()
    string(APPEND CONAN_C_FLAGS " ${BITCODE} ${VISIBILITY}")
    string(APPEND CONAN_CXX_FLAGS " ${BITCODE} ${VISIBILITY}")
    # Objective-C/C++ specific flags
    string(APPEND CONAN_OBJC_FLAGS " ${BITCODE} ${VISIBILITY} ${FOBJC_ARC}")
    string(APPEND CONAN_OBJCXX_FLAGS " ${BITCODE} ${VISIBILITY} ${FOBJC_ARC}")
endif()


########## 'rpath_link_flags' block #############
# Pass -rpath-link pointing to all directories with runtime libraries


########## 'libcxx' block #############
# Definition of libcxx from 'compiler.libcxx' setting, defining the
# right CXX_FLAGS for that libcxx

message(STATUS "Conan toolchain: Defining libcxx as C++ flags: -stdlib=libc++")
string(APPEND CONAN_CXX_FLAGS " -stdlib=libc++")


########## 'cppstd' block #############
# Define the C++ and C standards from 'compiler.cppstd' and 'compiler.cstd'

function(conan_modify_std_watch variable access value current_list_file stack)
    set(conan_watched_std_variable "17")
    if (${variable} STREQUAL "CMAKE_C_STANDARD")
        set(conan_watched_std_variable "")
    endif()
    if ("${access}" STREQUAL "MODIFIED_ACCESS" AND NOT "${value}" STREQUAL "${conan_watched_std_variable}")
        message(STATUS "Warning: Standard ${variable} value defined in conan_toolchain.cmake to ${conan_watched_std_variable} has been modified to ${value} by ${current_list_file}")
    endif()
    unset(conan_watched_std_variable)
endfunction()

message(STATUS "Conan toolchain: C++ Standard 17 with extensions ON")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
variable_watch(CMAKE_CXX_STANDARD conan_modify_std_watch)


########## 'extra_flags' block #############
# Include extra C++, C and linker flags from configuration tools.build:<type>flags
# and from CMakeToolchain.extra_<type>_flags

# Conan conf flags start: 
# Conan conf flags end


########## 'cmake_flags_init' block #############
# Define CMAKE_<XXX>_FLAGS from CONAN_<XXX>_FLAGS

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER ${config} config)
    if(DEFINED CONAN_CXX_FLAGS_${config})
      string(APPEND CMAKE_CXX_FLAGS_${config}_INIT " ${CONAN_CXX_FLAGS_${config}}")
    endif()
    if(DEFINED CONAN_C_FLAGS_${config})
      string(APPEND CMAKE_C_FLAGS_${config}_INIT " ${CONAN_C_FLAGS_${config}}")
    endif()
    if(DEFINED CONAN_SHARED_LINKER_FLAGS_${config})
      string(APPEND CMAKE_SHARED_LINKER_FLAGS_${config}_INIT " ${CONAN_SHARED_LINKER_FLAGS_${config}}")
    endif()
    if(DEFINED CONAN_EXE_LINKER_FLAGS_${config})
      string(APPEND CMAKE_EXE_LINKER_FLAGS_${config}_INIT " ${CONAN_EXE_LINKER_FLAGS_${config}}")
    endif()
    if(DEFINED CONAN_RC_FLAGS_${config})
      string(APPEND CMAKE_RC_FLAGS_${config}_INIT " ${CONAN_RC_FLAGS_${config}}")
    endif()
endforeach()

if(DEFINED CONAN_CXX_FLAGS)
  string(APPEND CMAKE_CXX_FLAGS_INIT " ${CONAN_CXX_FLAGS}")
endif()
if(DEFINED CONAN_C_FLAGS)
  string(APPEND CMAKE_C_FLAGS_INIT " ${CONAN_C_FLAGS}")
endif()
if(DEFINED CONAN_SHARED_LINKER_FLAGS)
  string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${CONAN_SHARED_LINKER_FLAGS}")
endif()
if(DEFINED CONAN_EXE_LINKER_FLAGS)
  string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${CONAN_EXE_LINKER_FLAGS}")
endif()
if(DEFINED CONAN_RC_FLAGS)
  string(APPEND CMAKE_RC_FLAGS_INIT " ${CONAN_RC_FLAGS}")
endif()
if(DEFINED CONAN_OBJCXX_FLAGS)
  string(APPEND CMAKE_OBJCXX_FLAGS_INIT " ${CONAN_OBJCXX_FLAGS}")
endif()
if(DEFINED CONAN_OBJC_FLAGS)
  string(APPEND CMAKE_OBJC_FLAGS_INIT " ${CONAN_OBJC_FLAGS}")
endif()


########## 'extra_variables' block #############
# Definition of extra CMake variables from tools.cmake.cmaketoolchain:extra_variables



########## 'try_compile' block #############
# Blocks after this one will not be added when running CMake try/checks
get_property( _CMAKE_IN_TRY_COMPILE GLOBAL PROPERTY IN_TRY_COMPILE )
if(_CMAKE_IN_TRY_COMPILE)
    message(STATUS "Running toolchain IN_TRY_COMPILE")
    return()
endif()


########## 'find_paths' block #############
# Define paths to find packages, programs, libraries, etc.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/conan_cmakedeps_paths.cmake")
  message(STATUS "Conan toolchain: Including CMakeConfigDeps generated conan_cmakedeps_paths.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/conan_cmakedeps_paths.cmake")
else()

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

# Definition of CMAKE_MODULE_PATH
list(PREPEND CMAKE_MODULE_PATH "/Users/ayx106047/.conan2/p/opensf16b602a2157d/p/lib/cmake")
# the generators folder (where conan generates files, like this toolchain)
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})

# Definition of CMAKE_PREFIX_PATH, CMAKE_XXXXX_PATH
# The explicitly defined "builddirs" of "host" context dependencies must be in PREFIX_PATH
list(PREPEND CMAKE_PREFIX_PATH "/Users/ayx106047/.conan2/p/opensf16b602a2157d/p/lib/cmake")
# The Conan local "generators" folder, where this toolchain is saved.
list(PREPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_LIST_DIR} )
list(PREPEND CMAKE_LIBRARY_PATH "/Users/ayx106047/.conan2/p/lua2b301a8475149/p/lib" "/Users/ayx106047/.conan2/p/b/sdl_i22d0fae4e874c/p/lib" "/Users/ayx106047/.conan2/p/libtica5dae7248a26/p/lib" "/Users/ayx106047/.conan2/p/xz_utfcd3c9fb50ebf/p/lib" "/Users/ayx106047/.conan2/p/b/libjpbc23fec503d7a/p/lib" "/Users/ayx106047/.conan2/p/b/libwe414de368a2cc2/p/lib" "/Users/ayx106047/.conan2/p/b/sdl_t30284a25207d5/p/lib" "/Users/ayx106047/.conan2/p/b/freet5cb34600875a3/p/lib" "/Users/ayx106047/.conan2/p/libpnb818f70e375af/p/lib" "/Users/ayx106047/.conan2/p/bzip267e5f750bcd66/p/lib" "/Users/ayx106047/.conan2/p/brotl81bca95eff9b8/p/lib" "/Users/ayx106047/.conan2/p/b/sdl_m87d0381e9b5bd/p/lib" "/Users/ayx106047/.conan2/p/b/sdldeb6b90cf1225/p/lib" "/Users/ayx106047/.conan2/p/flaca5a989a8d3993/p/lib" "/Users/ayx106047/.conan2/p/mpg12ff948303407c6/p/lib" "lib" "/Users/ayx106047/.conan2/p/opusff7f459f0af8e8/p/lib" "/Users/ayx106047/.conan2/p/b/opusd3dc91dc2c079/p/lib" "/Users/ayx106047/.conan2/p/opensf16b602a2157d/p/lib" "/Users/ayx106047/.conan2/p/zlibf7af9c3d20ae6/p/lib" "/Users/ayx106047/.conan2/p/b/libmofa04556b8dfb8/p/lib" "/Users/ayx106047/.conan2/p/b/ogg55c90500b14d6/p/lib" "/Users/ayx106047/.conan2/p/imgui71e3a1b51896e/p/lib")
list(PREPEND CMAKE_INCLUDE_PATH "/Users/ayx106047/.conan2/p/lua2b301a8475149/p/include" "/Users/ayx106047/.conan2/p/b/sdl_i22d0fae4e874c/p/include" "/Users/ayx106047/.conan2/p/b/sdl_i22d0fae4e874c/p/include/SDL2" "/Users/ayx106047/.conan2/p/libtica5dae7248a26/p/include" "/Users/ayx106047/.conan2/p/xz_utfcd3c9fb50ebf/p/include" "/Users/ayx106047/.conan2/p/b/libjpbc23fec503d7a/p/include" "/Users/ayx106047/.conan2/p/b/libwe414de368a2cc2/p/include" "/Users/ayx106047/.conan2/p/b/sdl_t30284a25207d5/p/include" "/Users/ayx106047/.conan2/p/b/sdl_t30284a25207d5/p/include/SDL2" "/Users/ayx106047/.conan2/p/b/freet5cb34600875a3/p/include" "/Users/ayx106047/.conan2/p/b/freet5cb34600875a3/p/include/freetype2" "/Users/ayx106047/.conan2/p/libpnb818f70e375af/p/include" "/Users/ayx106047/.conan2/p/bzip267e5f750bcd66/p/include" "/Users/ayx106047/.conan2/p/brotl81bca95eff9b8/p/include" "/Users/ayx106047/.conan2/p/b/sdl_m87d0381e9b5bd/p/include" "/Users/ayx106047/.conan2/p/b/sdl_m87d0381e9b5bd/p/include/SDL2" "/Users/ayx106047/.conan2/p/b/sdldeb6b90cf1225/p/include" "/Users/ayx106047/.conan2/p/b/sdldeb6b90cf1225/p/include/SDL2" "/Users/ayx106047/.conan2/p/flaca5a989a8d3993/p/include" "/Users/ayx106047/.conan2/p/mpg12ff948303407c6/p/include" "include" "/Users/ayx106047/.conan2/p/opusff7f459f0af8e8/p/include" "/Users/ayx106047/.conan2/p/opusff7f459f0af8e8/p/include/opus" "/Users/ayx106047/.conan2/p/b/opusd3dc91dc2c079/p/include" "/Users/ayx106047/.conan2/p/b/opusd3dc91dc2c079/p/include/opus" "/Users/ayx106047/.conan2/p/opensf16b602a2157d/p/include" "/Users/ayx106047/.conan2/p/zlibf7af9c3d20ae6/p/include" "/Users/ayx106047/.conan2/p/b/libmofa04556b8dfb8/p/include" "/Users/ayx106047/.conan2/p/b/libmofa04556b8dfb8/p/include/libmodplug" "/Users/ayx106047/.conan2/p/b/ogg55c90500b14d6/p/include" "/Users/ayx106047/.conan2/p/imgui71e3a1b51896e/p/include" "/Users/ayx106047/.conan2/p/glmc7261f667dea4/p/include")
set(CONAN_RUNTIME_LIB_DIRS "/Users/ayx106047/.conan2/p/lua2b301a8475149/p/lib" "/Users/ayx106047/.conan2/p/b/sdl_i22d0fae4e874c/p/lib" "/Users/ayx106047/.conan2/p/libtica5dae7248a26/p/lib" "/Users/ayx106047/.conan2/p/xz_utfcd3c9fb50ebf/p/lib" "/Users/ayx106047/.conan2/p/b/libjpbc23fec503d7a/p/lib" "/Users/ayx106047/.conan2/p/b/libwe414de368a2cc2/p/lib" "/Users/ayx106047/.conan2/p/b/sdl_t30284a25207d5/p/lib" "/Users/ayx106047/.conan2/p/b/freet5cb34600875a3/p/lib" "/Users/ayx106047/.conan2/p/libpnb818f70e375af/p/lib" "/Users/ayx106047/.conan2/p/bzip267e5f750bcd66/p/lib" "/Users/ayx106047/.conan2/p/brotl81bca95eff9b8/p/lib" "/Users/ayx106047/.conan2/p/b/sdl_m87d0381e9b5bd/p/lib" "/Users/ayx106047/.conan2/p/b/sdldeb6b90cf1225/p/lib" "/Users/ayx106047/.conan2/p/flaca5a989a8d3993/p/lib" "/Users/ayx106047/.conan2/p/mpg12ff948303407c6/p/lib" "lib" "/Users/ayx106047/.conan2/p/opusff7f459f0af8e8/p/lib" "/Users/ayx106047/.conan2/p/b/opusd3dc91dc2c079/p/lib" "/Users/ayx106047/.conan2/p/opensf16b602a2157d/p/lib" "/Users/ayx106047/.conan2/p/zlibf7af9c3d20ae6/p/lib" "/Users/ayx106047/.conan2/p/b/libmofa04556b8dfb8/p/lib" "/Users/ayx106047/.conan2/p/b/ogg55c90500b14d6/p/lib" "/Users/ayx106047/.conan2/p/imgui71e3a1b51896e/p/lib" )

endif()


########## 'pkg_config' block #############
# Define pkg-config from 'tools.gnu:pkg_config' executable and paths

if (DEFINED ENV{PKG_CONFIG_PATH})
set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_LIST_DIR}:$ENV{PKG_CONFIG_PATH}")
else()
set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_LIST_DIR}:")
endif()


########## 'rpath' block #############
# Defining CMAKE_SKIP_RPATH



########## 'output_dirs' block #############
# Definition of CMAKE_INSTALL_XXX folders

# Ensure export(PACKAGE) honors CMAKE_EXPORT_PACKAGE_REGISTRY even if the
# project sets cmake_minimum_required() lower than 3.15.
cmake_policy(SET CMP0090 NEW)
if(NOT DEFINED CMAKE_EXPORT_PACKAGE_REGISTRY)
    set(CMAKE_EXPORT_PACKAGE_REGISTRY OFF)
endif()

set(CMAKE_INSTALL_BINDIR "bin")
set(CMAKE_INSTALL_SBINDIR "bin")
set(CMAKE_INSTALL_LIBEXECDIR "bin")
set(CMAKE_INSTALL_LIBDIR "lib")
set(CMAKE_INSTALL_INCLUDEDIR "include")
set(CMAKE_INSTALL_OLDINCLUDEDIR "include")


########## 'variables' block #############
# Definition of CMake variables from CMakeToolchain.variables values

# Variables
# Variables  per configuration



########## 'preprocessor' block #############
# Preprocessor definitions from CMakeToolchain.preprocessor_definitions values

# Preprocessor definitions per configuration



if(CMAKE_POLICY_DEFAULT_CMP0091)  # Avoid unused and not-initialized warnings
endif()

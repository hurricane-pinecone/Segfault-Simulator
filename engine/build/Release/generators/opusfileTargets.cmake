# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/opusfile-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${opusfile_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${opusfile_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET opusfile::opusfile)
    add_library(opusfile::opusfile INTERFACE IMPORTED)
    message(${opusfile_MESSAGE_MODE} "Conan: Target declared 'opusfile::opusfile'")
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/opusfile-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()
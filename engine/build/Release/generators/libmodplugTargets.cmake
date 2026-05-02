# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/libmodplug-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${libmodplug_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${libmodplug_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET libmodplug::libmodplug)
    add_library(libmodplug::libmodplug INTERFACE IMPORTED)
    message(${libmodplug_MESSAGE_MODE} "Conan: Target declared 'libmodplug::libmodplug'")
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/libmodplug-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()
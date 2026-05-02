# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/TIFF-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${libtiff_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${TIFF_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET TIFF::TIFF)
    add_library(TIFF::TIFF INTERFACE IMPORTED)
    message(${TIFF_MESSAGE_MODE} "Conan: Target declared 'TIFF::TIFF'")
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/TIFF-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()
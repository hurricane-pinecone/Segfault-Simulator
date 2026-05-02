# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/SDL2_ttf-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${sdl_ttf_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${SDL2_ttf_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET sdl_ttf::sdl_ttf)
    add_library(sdl_ttf::sdl_ttf INTERFACE IMPORTED)
    message(${SDL2_ttf_MESSAGE_MODE} "Conan: Target declared 'sdl_ttf::sdl_ttf'")
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/SDL2_ttf-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()
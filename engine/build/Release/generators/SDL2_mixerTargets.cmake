# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/SDL2_mixer-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${sdl_mixer_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${SDL2_mixer_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET SDL2_mixer::SDL2_mixer)
    add_library(SDL2_mixer::SDL2_mixer INTERFACE IMPORTED)
    message(${SDL2_mixer_MESSAGE_MODE} "Conan: Target declared 'SDL2_mixer::SDL2_mixer'")
endif()
if(NOT TARGET SDL2_mixer::SDL2_mixer-static)
    add_library(SDL2_mixer::SDL2_mixer-static INTERFACE IMPORTED)
    set_property(TARGET SDL2_mixer::SDL2_mixer-static PROPERTY INTERFACE_LINK_LIBRARIES SDL2_mixer::SDL2_mixer)
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/SDL2_mixer-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()
# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/SDL2_image-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${sdl_image_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${SDL2_image_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET SDL2_image::SDL2_image)
    add_library(SDL2_image::SDL2_image INTERFACE IMPORTED)
    message(${SDL2_image_MESSAGE_MODE} "Conan: Target declared 'SDL2_image::SDL2_image'")
endif()
if(NOT TARGET SDL2_image::SDL2_image-static)
    add_library(SDL2_image::SDL2_image-static INTERFACE IMPORTED)
    set_property(TARGET SDL2_image::SDL2_image-static PROPERTY INTERFACE_LINK_LIBRARIES SDL2_image::SDL2_image)
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/SDL2_image-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()
file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(sampleGame
    ${GAME_SRC}
)

target_include_directories(sampleGame PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(sampleGame PRIVATE
    engine
)

set_target_properties(sampleGame PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(EMSCRIPTEN)
    set_target_properties(sampleGame PROPERTIES
        SUFFIX ".html"
    )

    target_link_options(sampleGame PRIVATE
        --preload-file "${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
        -sALLOW_MEMORY_GROWTH=1
        -sASYNCIFY
    )
else()
    add_custom_command(TARGET sampleGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:sampleGame>/assets"
    )

    add_custom_target(run
        COMMAND $<TARGET_FILE:sampleGame>
        DEPENDS sampleGame
        WORKING_DIRECTORY $<TARGET_FILE_DIR:sampleGame>
    )
endif()

file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(game
    ${GAME_SRC}
)

target_include_directories(game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(game PRIVATE
    engine
)

set_target_properties(game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_target(copy_game_assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:game>/assets"
    DEPENDS game
)

add_custom_target(run
    COMMAND $<TARGET_FILE:game>
    DEPENDS copy_game_assets
    WORKING_DIRECTORY $<TARGET_FILE_DIR:game>
)

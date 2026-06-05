# Runtime tests link the full engine (SDL/OpenGL/imgui).

add_executable(flatRenderSmokeTest rendering/flatRenderSmokeTest.cpp)
target_link_libraries(flatRenderSmokeTest PRIVATE engine)
add_test(NAME flatRenderSmokeTest COMMAND flatRenderSmokeTest)

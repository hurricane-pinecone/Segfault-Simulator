add_executable(luaTests scripting/luaTests.cpp)
target_link_libraries(luaTests PRIVATE engine-core)

add_test(NAME luaTests COMMAND luaTests)

add_executable(flatProjectionTests rendering/flatProjectionTests.cpp)
target_link_libraries(flatProjectionTests PRIVATE engine-core)

add_test(NAME flatProjectionTests COMMAND flatProjectionTests)

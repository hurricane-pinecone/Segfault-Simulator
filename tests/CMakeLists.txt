add_executable(luaTests scripting/luaTests.cpp)
target_link_libraries(luaTests PRIVATE engine)

add_test(NAME luaTests COMMAND luaTests)

# Installs the engine SDK (cmake --install BUILD --prefix PREFIX) for game modules, then gives every file it actually
# copied (not those already up to date) the time it was installed. cmake --install keeps each source file's own
# time: a header changed before a game module was last compiled, but installed after, would look older than the
# module's objects, and they'd never be rebuilt against it (a component's new layout read with the old one).
#
#   cmake -DBUILD=<engine build dir> -DPREFIX=<sdk dir> -P installSdk.cmake
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${PREFIX}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE result)
message("${out}${err}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installing the engine SDK failed (${result})")
endif()
string(REGEX MATCHALL "-- Installing: [^\n]+" installed "${out}")
foreach(line IN LISTS installed)
    string(REPLACE "-- Installing: " "" file "${line}")
    file(TOUCH_NOCREATE "${file}")
endforeach()

# Tests mirror the library split: core/ tests link engine-core (no third-party
# deps); runtime/ tests link the full engine (SDL/imgui) and only build when the
# render library is present; webgpu/ tests link engine-webgpu and need a GPU
# device (they skip without one).

# On Windows a program finds its DLLs (the engine's, SDL's, MinGW's
# libwinpthread) only beside it, so the tests are built into bin/ with them.
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_subdirectory(core)

if(NOT ENGINE_CORE_ONLY)
    add_subdirectory(runtime)
endif()

if(TARGET engine-webgpu)
    add_subdirectory(webgpu)
endif()

if(TARGET sfs-codegen-lib)
    add_subdirectory(tools)
endif()

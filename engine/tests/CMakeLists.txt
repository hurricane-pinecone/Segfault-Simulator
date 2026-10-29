# Tests mirror the library split: core/ tests link engine-core (no third-party
# deps); runtime/ tests link the full engine (SDL/OpenGL) and only build when the
# render library is present; webgpu/ tests link engine-webgpu and need a GPU
# device (they skip without one).
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

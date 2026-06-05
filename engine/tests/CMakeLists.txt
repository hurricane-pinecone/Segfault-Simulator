# Tests mirror the library split: core/ tests link engine-core (no third-party
# deps); runtime/ tests link the full engine (SDL/OpenGL) and only build when the
# render library is present.
add_subdirectory(core)

if(NOT ENGINE_CORE_ONLY)
    add_subdirectory(runtime)
endif()

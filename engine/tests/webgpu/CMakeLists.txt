# WebGPU tests link engine-webgpu and run on a real device, made headless (no
# window). A machine with no usable adapter (no GPU driver, e.g. a bare CI
# runner) skips them: they exit with 77, which CTest reports as skipped.
function(add_webgpu_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE engine-webgpu)
    target_compile_definitions(${name} PRIVATE SDL_MAIN_HANDLED)
    if(WIN32)
        target_sources(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/sdlMainStub.cpp)
    endif()
    sfs_deploy_runtime(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS webgpu SKIP_RETURN_CODE 77)
endfunction()

add_webgpu_test(webGpuRendererTests webGpuRendererTests.cpp)

# The same tests on wgpu's OpenGL fallback, which runs the engine on machines
# without Vulkan, Direct3D 12 or Metal (skipped where there's no GL either).
add_test(NAME webGpuRendererTestsOnOpenGL COMMAND webGpuRendererTests)
set_tests_properties(webGpuRendererTestsOnOpenGL PROPERTIES
    LABELS webgpu SKIP_RETURN_CODE 77 ENVIRONMENT SFS_WGPU_BACKEND=gl)

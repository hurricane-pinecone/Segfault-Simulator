import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout


class SfsEngineConan(ConanFile):
    """Conan recipe for the SegFaultSimulator engine.

    Serves two roles:
      * `conan create .`  -> builds and packages the engine so another project
        can `requires = "sfs-engine/<version>"` and link sfs::engine /
        sfs::engine-core, with the SDL/imgui/GLEW closure resolved for them.
      * `conan install .` -> installs the same dependency closure for an in-tree
        workspace build (the existing dev flow; replaces the old conanfile.txt).
    """

    name = "sfs-engine"
    version = "0.1.0"
    description = (
        "SegFaultSimulator: a 2.5D isometric heightfield game engine "
        "(dependency-free ECS/scripting core + SDL/OpenGL render runtime)."
    )
    license = "MIT"
    settings = "os", "compiler", "build_type", "arch"

    # core_only packages just engine-core (ECS/scripting/particles), without the
    # render runtime or any of its third-party dependencies.
    options = {"core_only": [True, False]}
    default_options = {
        "core_only": False,
        # imgui must be built with the SDL2 + OpenGL3 backends the runtime uses.
        "imgui/*:with_sdl2": True,
        "imgui/*:with_opengl3": True,
        "imgui/*:with_sdlrenderer2": False,
    }

    # Everything the package build needs; the sample game and its assets are not
    # part of the engine package (the recipe configures with ENGINE_BUILD_SAMPLE
    # OFF, so the sampleGame subdirectory is never added).
    exports_sources = (
        "CMakeLists.txt",
        "engine/CMakeLists.txt",
        "engine/cmake/*",
        "engine/include/*",
        "engine/src/*",
        "engine/lib/*",
        "engine/tests/*",
    )

    def requirements(self):
        # engine-core links nothing third-party; only the render runtime needs
        # the desktop stack.
        if not self.options.core_only:
            self.requires("sdl/2.28.3")
            self.requires("sdl_image/2.8.2")
            self.requires("sdl_ttf/2.22.0")
            self.requires("sdl_mixer/2.8.0")
            self.requires("imgui/1.92.7")
            self.requires("glew/2.2.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        # generate() runs for both `conan install` (in-tree dev) and packaging,
        # so it must stay neutral: no engine-build overrides leak into the dev
        # workspace toolchain. The packaging-only flags are passed in build().
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

    def build(self):
        cmake = CMake(self)
        # Package build: engine libs only (no sample), and let Conan own the
        # CMake package config (skip the hand-written one, which targets raw
        # `cmake --install` consumers).
        cmake.configure(variables={
            "ENGINE_CORE_ONLY": self.options.core_only,
            "ENGINE_BUILD_SAMPLE": False,
            "ENGINE_INSTALL_CMAKE_PACKAGE": False,
        })
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "engine")
        # Auto-included by Conan's generated config; sets ENGINE_ASSET_DIR so
        # consumers can stage the engine's runtime assets (matches the raw-install
        # find_package behaviour).
        self.cpp_info.set_property(
            "cmake_build_modules",
            [os.path.join("share", "sfs-engine", "cmake", "engineAssets.cmake")],
        )

        core = self.cpp_info.components["engine-core"]
        core.set_property("cmake_target_name", "sfs::engine-core")
        core.libs = ["engine-core"]
        # Mirror the install-interface include roots from engine/CMakeLists.txt.
        core.includedirs = ["include", "include/lib", "include/lib/glm"]
        if self.settings.os in ("Linux", "FreeBSD"):
            core.system_libs.append("pthread")

        if not self.options.core_only:
            rt = self.cpp_info.components["engine"]
            rt.set_property("cmake_target_name", "sfs::engine")
            rt.libs = ["engine"]
            rt.includedirs = [
                "include",
                "include/lib",
                "include/lib/imgui",
                "include/lib/tracy/public",
            ]
            # Static-lib link order: engine before engine-core before the deps.
            rt.requires = [
                "engine-core",
                "sdl::sdl",
                "sdl_image::sdl_image",
                "sdl_ttf::sdl_ttf",
                "sdl_mixer::sdl_mixer",
                "imgui::imgui",
                "glew::glew",
            ]
            if self.settings.os == "Macos":
                rt.frameworks = ["OpenGL"]
            elif self.settings.os in ("Linux", "FreeBSD"):
                rt.system_libs.append("GL")

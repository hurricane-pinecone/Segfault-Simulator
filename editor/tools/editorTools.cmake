# The Code panel's tools, shipped with the editor so its Neovim needs nothing installed: Neovim, ripgrep (live search
# and find and replace) and clangd (C++ language server). Pinned official release archives (see VENDORED.md beside
# this file, which also says how to update them), checked against their SHA-256, downloaded once into
# SFS_EDITOR_TOOLS_CACHE when the editor is configured, and extracted into tools/ beside the editor executable when
# it builds:
#
#   tools/nvim/       bin/nvim, lib/, share/nvim/runtime, LICENSE.txt
#   tools/ripgrep/    rg, COPYING, LICENSE-MIT, UNLICENSE
#   tools/clangd/     bin/clangd, lib/clang/<version>/include, LICENSE.TXT
#
# NvimClient (editor/src/nvim/nvimClient.cpp) starts tools/nvim/bin/nvim with these folders first on PATH. A
# Windows editor cross-built from WSL runs Neovim and its tools in WSL instead, so nothing is bundled there.
#
# Included by editor/CMakeLists.txt once SFS_EDITOR_BUILD_RUNNER is set.

set(SFS_EDITOR_NVIM_VERSION 0.12.6)
set(SFS_EDITOR_RIPGREP_VERSION 15.2.0)
set(SFS_EDITOR_CLANGD_VERSION 23.1.0)

set(nvimReleases "https://github.com/neovim/neovim/releases/download/v${SFS_EDITOR_NVIM_VERSION}")
set(ripgrepReleases "https://github.com/BurntSushi/ripgrep/releases/download/${SFS_EDITOR_RIPGREP_VERSION}")
set(clangdReleases "https://github.com/clangd/clangd/releases/download/${SFS_EDITOR_CLANGD_VERSION}")

# Neovim's release archives don't carry its licence: it comes from the release's tag.
set(nvimLicenseUrl "https://raw.githubusercontent.com/neovim/neovim/v${SFS_EDITOR_NVIM_VERSION}/LICENSE.txt")
set(nvimLicenseSha256 de23202ef9a51f5e654034190539739a4abece5505f2ce75e780791106707011)

# On by default for native builds in a release configuration (what ships). Debug trees use the tools on PATH, as do
# offline builds and CI jobs that turn this off.
get_property(sfsEditorMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(SFS_EDITOR_BUILD_RUNNER STREQUAL "native"
   AND (sfsEditorMultiConfig OR CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$"))
    set(sfsEditorBundleDefault ON)
else()
    set(sfsEditorBundleDefault OFF)
endif()
option(SFS_EDITOR_BUNDLE_TOOLS
    "Ship Neovim, ripgrep and clangd with the editor for its Code panel (downloads pinned releases)"
    ${sfsEditorBundleDefault})

# Shared by every build tree on the machine: the archives are checked against their pinned hashes before use.
if(NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(sfsEditorToolsCacheDefault "$ENV{XDG_CACHE_HOME}/sfs/editor-tools")
elseif(CMAKE_HOST_WIN32 AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/sfs/editor-tools" sfsEditorToolsCacheDefault)
elseif(NOT "$ENV{HOME}" STREQUAL "")
    set(sfsEditorToolsCacheDefault "$ENV{HOME}/.cache/sfs/editor-tools")
else()
    set(sfsEditorToolsCacheDefault "${CMAKE_BINARY_DIR}/_downloads/editor-tools")
endif()
set(SFS_EDITOR_TOOLS_CACHE "${sfsEditorToolsCacheDefault}" CACHE PATH
    "Where the editor's bundled tools are downloaded to (shared by build trees)")

# Which release archives this editor's platform takes.
set(sfsEditorToolsPlatform "")
if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(sfsEditorToolsPlatform "windows-x64")
    set(nvimArchive nvim-win64.zip)
    set(nvimSha256 4e12b7103b601ffc8f25954b4da100f339faa73dccb69a5b135e897fa3d893ed)
    set(ripgrepArchive ripgrep-${SFS_EDITOR_RIPGREP_VERSION}-x86_64-pc-windows-msvc.zip)
    set(ripgrepSha256 71b2fef860abe467217a538ff31de02f5258807c0129f771846f87bd029aafc5)
    set(clangdArchive clangd-windows-${SFS_EDITOR_CLANGD_VERSION}.zip)
    set(clangdSha256 23412a240756a162e7b98a282f36aa2a23a88db5ce16a0cbc4fef7253768c810)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(sfsEditorToolsPlatform "linux-x64")
    set(nvimArchive nvim-linux-x86_64.tar.gz)
    set(nvimSha256 474430d53e6264f6d6dd18db42d6dc9df3a1b56ca9e88a325bbf860e1a811d87)
    set(ripgrepArchive ripgrep-${SFS_EDITOR_RIPGREP_VERSION}-x86_64-unknown-linux-musl.tar.gz)
    set(ripgrepSha256 33e15bcf1624b25cdd2a55813a47a2f95dbe126268203e76aa6a585d1e7b149c)
    set(clangdArchive clangd-linux-${SFS_EDITOR_CLANGD_VERSION}.zip)
    set(clangdSha256 e53b1a96196095faedb7642cf64964f7fb9ad4a0c1f00dd2c172a3d9dcbafdfd)
elseif(APPLE)
    # clangd's macOS build is universal; Neovim and ripgrep have one archive per architecture.
    set(sfsEditorMacArch "${CMAKE_OSX_ARCHITECTURES}")
    if(sfsEditorMacArch STREQUAL "")
        set(sfsEditorMacArch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(clangdArchive clangd-mac-${SFS_EDITOR_CLANGD_VERSION}.zip)
    set(clangdSha256 1082e6638223b785ca2daf0939f13afcd0bb95c84ee9a4bbaff4745365159253)
    if(sfsEditorMacArch STREQUAL "arm64")
        set(sfsEditorToolsPlatform "macos-arm64")
        set(nvimArchive nvim-macos-arm64.tar.gz)
        set(nvimSha256 1dbd148222b051ba6c307c4d17a318ed083df018a4f6a5a0ce46955ffc9ab87b)
        set(ripgrepArchive ripgrep-${SFS_EDITOR_RIPGREP_VERSION}-aarch64-apple-darwin.tar.gz)
        set(ripgrepSha256 3750b2e93f37e0c692657da574d7019a101c0084da05a790c83fd335bad973e4)
    elseif(sfsEditorMacArch STREQUAL "x86_64")
        set(sfsEditorToolsPlatform "macos-x64")
        set(nvimArchive nvim-macos-x86_64.tar.gz)
        set(nvimSha256 481424f1dbc85f637d57e47d27d042aa9c883e5a4fe9ee64fc4ee9573b221998)
        set(ripgrepArchive ripgrep-${SFS_EDITOR_RIPGREP_VERSION}-x86_64-apple-darwin.tar.gz)
        set(ripgrepSha256 af7825fcc69a2afc7a7aea55fc9af90e26421d8f20fe59df32e233c0b8a231c1)
    endif()
endif()

# Downloads `url` into `folder` (unless it's already there with the pinned hash) and sets `out` to the file.
function(sfs_editor_tools_download url sha256 folder out)
    get_filename_component(name "${url}" NAME)
    set(file "${folder}/${name}")

    if(EXISTS "${file}")
        file(SHA256 "${file}" actual)
        if(actual STREQUAL sha256)
            set(${out} "${file}" PARENT_SCOPE)
            return()
        endif()
    endif()

    message(STATUS "Downloading ${url} (the editor's Code panel tools)")
    file(DOWNLOAD "${url}" "${file}.part" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        list(GET status 1 reason)
        file(REMOVE "${file}.part")
        message(FATAL_ERROR "Couldn't download ${url}: ${reason}. Configure with -DSFS_EDITOR_BUNDLE_TOOLS=OFF to build "
                            "the editor without its tools: the Code panel then uses Neovim, ripgrep and clangd from PATH.")
    endif()
    file(RENAME "${file}.part" "${file}")
    set(${out} "${file}" PARENT_SCOPE)
endfunction()

# Downloads one tool's release archive from `url` (and a licence file to put beside it) and extracts it into
# tools/<folder> beside the editor whenever the archive changes. Appends its stamp to sfsEditorToolStamps.
function(sfs_editor_tool folder version url sha256)
    cmake_parse_arguments(PARSE_ARGV 4 arg "" "LICENSE_URL;LICENSE_SHA256" "")
    set(cache "${SFS_EDITOR_TOOLS_CACHE}/${folder}-${version}")
    sfs_editor_tools_download("${url}" "${sha256}" "${cache}" archiveFile)

    set(licenseArgs "")
    set(licenseFile "")
    if(arg_LICENSE_URL)
        sfs_editor_tools_download("${arg_LICENSE_URL}" "${arg_LICENSE_SHA256}" "${cache}" licenseFile)
        set(licenseArgs "-DLICENSE=${licenseFile}")
    endif()

    set(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/extractEditorTool.cmake")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/tools/$<CONFIG>/${folder}.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND ${CMAKE_COMMAND} "-DARCHIVE=${archiveFile}" "-DDEST=$<TARGET_FILE_DIR:SfsEditor>/tools/${folder}"
                ${licenseArgs} -P "${script}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/tools/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS "${archiveFile}" ${licenseFile} "${script}"
        COMMENT "Extracting ${folder} ${version} into the editor's tools/"
        VERBATIM)
    set(sfsEditorToolStamps ${sfsEditorToolStamps} "${stamp}" PARENT_SCOPE)
endfunction()

if(SFS_EDITOR_BUNDLE_TOOLS AND NOT SFS_EDITOR_BUILD_RUNNER STREQUAL "native")
    message(STATUS "SfsEditor: not bundling the Code panel's tools (Neovim and its tools run in WSL)")
elseif(SFS_EDITOR_BUNDLE_TOOLS AND sfsEditorToolsPlatform STREQUAL "")
    message(WARNING "SfsEditor: no bundled Code panel tools for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}; "
                    "the Code panel uses Neovim, ripgrep and clangd from PATH")
elseif(SFS_EDITOR_BUNDLE_TOOLS)
    message(STATUS "SfsEditor: bundling Neovim ${SFS_EDITOR_NVIM_VERSION}, ripgrep ${SFS_EDITOR_RIPGREP_VERSION} and "
                   "clangd ${SFS_EDITOR_CLANGD_VERSION} (${sfsEditorToolsPlatform})")
    set(sfsEditorToolStamps "")
    sfs_editor_tool(nvim ${SFS_EDITOR_NVIM_VERSION} "${nvimReleases}/${nvimArchive}" ${nvimSha256}
        LICENSE_URL "${nvimLicenseUrl}" LICENSE_SHA256 ${nvimLicenseSha256})
    sfs_editor_tool(ripgrep ${SFS_EDITOR_RIPGREP_VERSION} "${ripgrepReleases}/${ripgrepArchive}" ${ripgrepSha256})
    sfs_editor_tool(clangd ${SFS_EDITOR_CLANGD_VERSION} "${clangdReleases}/${clangdArchive}" ${clangdSha256})
    add_custom_target(SfsEditorTools DEPENDS ${sfsEditorToolStamps})
    add_dependencies(SfsEditor SfsEditorTools)
elseif(SFS_EDITOR_BUILD_RUNNER STREQUAL "native")
    # Turned off after a build that had them: the editor would still start the old ones.
    add_custom_target(SfsEditorTools
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:SfsEditor>/tools"
                "${CMAKE_CURRENT_BINARY_DIR}/tools/$<CONFIG>"
        VERBATIM)
    add_dependencies(SfsEditor SfsEditorTools)
endif()

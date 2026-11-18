# Code panel tools (downloaded, pinned)

The editor's Code panel (embedded Neovim) ships with the tools its config uses, so a native editor needs nothing
installed. `editorTools.cmake` downloads the official release archives below when the editor is configured
(checking each against its SHA-256), keeps them in `SFS_EDITOR_TOOLS_CACHE` (by default `~/.cache/sfs/editor-tools`,
or `%LOCALAPPDATA%\sfs\editor-tools` on Windows, shared by every build tree), and the `SfsEditorTools` target extracts
them into `tools/` beside the editor executable:

| Folder | What | Licence (shipped in the folder) |
| --- | --- | --- |
| `tools/nvim/` | Neovim (`bin/nvim`, its runtime in `share/nvim/runtime`) | Apache 2.0, with parts under the Vim licence (`LICENSE.txt`) |
| `tools/ripgrep/` | ripgrep (`rg`): mini.pick's live search, grug-far's find and replace | MIT or Unlicense (`COPYING`, `LICENSE-MIT`, `UNLICENSE`) |
| `tools/clangd/` | clangd (`bin/clangd`, with clang's built-in headers in `lib/clang/`) | Apache 2.0 with LLVM exceptions (`LICENSE.TXT`) |

`SFS_EDITOR_BUNDLE_TOOLS` turns it on or off. It is on by default for native builds in a release configuration
(Release, RelWithDebInfo, MinSizeRel, or a multi-config generator such as Visual Studio's) and off for debug trees,
which use the tools on `PATH`. Turn it off (`-DSFS_EDITOR_BUNDLE_TOOLS=OFF`) to build offline; the build then removes
any `tools/` an earlier build left. A Windows editor cross-built from WSL runs Neovim and its tools in WSL, so nothing
is bundled there.

## Pins

Neovim 0.12.6, https://github.com/neovim/neovim/releases/tag/v0.12.6

| Platform | Archive | SHA-256 |
| --- | --- | --- |
| Windows x64 | `nvim-win64.zip` | `4e12b7103b601ffc8f25954b4da100f339faa73dccb69a5b135e897fa3d893ed` |
| Linux x64 | `nvim-linux-x86_64.tar.gz` | `474430d53e6264f6d6dd18db42d6dc9df3a1b56ca9e88a325bbf860e1a811d87` |
| macOS arm64 | `nvim-macos-arm64.tar.gz` | `1dbd148222b051ba6c307c4d17a318ed083df018a4f6a5a0ce46955ffc9ab87b` |
| macOS x64 | `nvim-macos-x86_64.tar.gz` | `481424f1dbc85f637d57e47d27d042aa9c883e5a4fe9ee64fc4ee9573b221998` |
| (all) | `LICENSE.txt` from the `v0.12.6` tag (the archives don't carry it) | `de23202ef9a51f5e654034190539739a4abece5505f2ce75e780791106707011` |

ripgrep 15.2.0, https://github.com/BurntSushi/ripgrep/releases/tag/15.2.0

| Platform | Archive | SHA-256 |
| --- | --- | --- |
| Windows x64 | `ripgrep-15.2.0-x86_64-pc-windows-msvc.zip` | `71b2fef860abe467217a538ff31de02f5258807c0129f771846f87bd029aafc5` |
| Linux x64 | `ripgrep-15.2.0-x86_64-unknown-linux-musl.tar.gz` | `33e15bcf1624b25cdd2a55813a47a2f95dbe126268203e76aa6a585d1e7b149c` |
| macOS arm64 | `ripgrep-15.2.0-aarch64-apple-darwin.tar.gz` | `3750b2e93f37e0c692657da574d7019a101c0084da05a790c83fd335bad973e4` |
| macOS x64 | `ripgrep-15.2.0-x86_64-apple-darwin.tar.gz` | `af7825fcc69a2afc7a7aea55fc9af90e26421d8f20fe59df32e233c0b8a231c1` |

clangd 23.1.0, https://github.com/clangd/clangd/releases/tag/23.1.0

| Platform | Archive | SHA-256 |
| --- | --- | --- |
| Windows x64 | `clangd-windows-23.1.0.zip` | `23412a240756a162e7b98a282f36aa2a23a88db5ce16a0cbc4fef7253768c810` |
| Linux x64 | `clangd-linux-23.1.0.zip` | `e53b1a96196095faedb7642cf64964f7fb9ad4a0c1f00dd2c172a3d9dcbafdfd` |
| macOS (universal) | `clangd-mac-23.1.0.zip` | `1082e6638223b785ca2daf0939f13afcd0bb95c84ee9a4bbaff4745365159253` |

Sizes extracted: Neovim about 40 MB, ripgrep about 5 MB, clangd about 100 MB on Windows and 225 MB on Linux (its
Linux build keeps its debug information).

## Updating

1. Pick the new release of each tool (Neovim stays 0.11 or later: `editor/nvim/init.lua` needs it).
2. Get each archive's SHA-256: the release page lists a digest per asset (`gh release view <tag> -R <repo> --json
   assets`), or download it and run `sha256sum`. For Neovim, also hash `LICENSE.txt` at the new tag.
3. Change the versions and hashes in `editorTools.cmake` and in the tables above.
4. Reconfigure and build `SfsEditor`: the new archives download once, and `tools/` is extracted again.
5. Run `nvimClientTests` with bundling on: its live checks drive the shipped Neovim and find the shipped `rg` and
   `clangd`.

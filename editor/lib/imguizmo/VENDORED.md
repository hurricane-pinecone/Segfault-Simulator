# ImGuizmo (vendored)

Source: https://github.com/CedricGuillemet/ImGuizmo, `src/ImGuizmo.{h,cpp}` at
commit `105e4fde45e4e74aae8973d98750820ef7aca7e2`. MIT licence (LICENSE).

Local change: `IMGUI_API` is renamed to `IMGUIZMO_API` (defined empty). ImGuizmo
is compiled into SfsEditor, while Dear ImGui is a shared library whose
`IMGUI_API` means "import from the imgui DLL" on Windows; keeping the original
macro would mislabel ImGuizmo's own functions as imported.

To update: re-download both files at a new commit and re-apply the rename
(`sed -i 's/\bIMGUI_API\b/IMGUIZMO_API/g' ImGuizmo.h ImGuizmo.cpp`, then restore
the empty `IMGUIZMO_API` define at the top of ImGuizmo.h).

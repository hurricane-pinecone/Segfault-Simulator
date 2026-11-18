# mini.nvim (vendored)

Modules of mini.nvim v0.18.0 (https://github.com/nvim-mini/mini.nvim, tag
`v0.18.0`, MIT: `LICENSE`), loaded by the editor's `init.lua` with
`packadd mini.nvim`: `lua/mini/{clue,files,hues,icons,pick}.lua` and their
`doc/` pages, copied unchanged except `icons.lua`.

`icons.lua` has its icons past U+FFFF (Material Design) taken out, because the
Code panel draws only the first 65,536 code points; `init.lua` names Nerd Font
icons for the common file types instead.

To update: copy the same files from the new release, then run
`python3 scripts/stripMiniIcons.py editor/nvim/pack/sfs/opt/mini.nvim/lua/mini/icons.lua`
from the SFS root.

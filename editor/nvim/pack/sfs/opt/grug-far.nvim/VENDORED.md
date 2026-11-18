# grug-far.nvim (vendored)

grug-far.nvim 1.6.80 (https://github.com/MagicDuck/grug-far.nvim, tag
`1.6.80`, MIT: `LICENSE`): `lua/`, `plugin/` and `doc/`, unchanged. The
editor's `init.lua` loads it with `packadd grug-far.nvim` and sets, through
its options, Nerd Font icons the Code panel can draw in place of its default
icons past U+FFFF.

It searches with ripgrep (`rg`), which must be on Neovim's PATH.

To update: copy the same folders and `LICENSE` from the new release, and check
its default icons (`lua/grug-far/opts.lua`) for new ones past U+FFFF.

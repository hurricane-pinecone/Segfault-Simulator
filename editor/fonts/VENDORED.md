# Code panel fonts (vendored)

Copied next to the editor executable (`fonts/`) by the SfsEditorFonts target.
The Code panel draws Neovim's grid with them (see `editor/src/widgets/codeFont.cpp`).

- `DepartureMono/`: Departure Mono 1.500 by Helena Zhang,
  https://github.com/rektdeckard/departure-mono release `v1.500`
  (`DepartureMono-1.500.zip`: `DepartureMono-Regular.otf`, `LICENSE`).
  SIL Open Font License 1.1. The panel's default font: a pixel font, drawn at
  whole multiples of its 11 px design size.
- `SymbolsNerdFont/`: Symbols Nerd Font Mono from Nerd Fonts 3.5.1,
  https://github.com/ryanoasis/nerd-fonts release `v3.5.1`
  (`NerdFontsSymbolsOnly.zip`: `SymbolsNerdFontMono-Regular.ttf`, `LICENSE`,
  `README.md`). MIT; `README.md` lists the icon sets inside and their own
  licences. Merged into the code font for file-type icons.

To update: download the new release archives and replace the files above.

-- sfs: SfsEditor's colour scheme for the Code panel, so Neovim looks like part
-- of the editor. Its colours are the editor's own (editor/src/widgets/
-- theme.cpp): warm charcoal greys with a hint of green, and sage for whatever
-- is selected, current or in focus. mini.hues (shipped beside init.lua) makes
-- the syntax colours around the editor's background and text; the groups that
-- should match the editor's own panels exactly are set after it.

-- mini.hues ships in the engine config's mini.nvim package, which init.lua
-- puts on 'packpath'.
pcall(vim.cmd.packadd, "mini.nvim")
local ok, hues = pcall(require, "mini.hues")
if not ok then
  vim.notify("sfs colour scheme: mini.hues is missing", vim.log.levels.WARN)
  return
end

local editor = {
  window = "#2e3130", -- panels
  popup = "#353837", -- child panes, popups
  deep = "#262827", -- title and menu bars
  field = "#3f4341", -- input fields
  text = "#e4e6e1",
  disabled = "#8c928c",
  line = "#4a4f4c", -- separators
  sage = "#aad6a0", -- the accent: lines and marks
  sage_dim = "#5c7a58", -- selections, with text on them
  sage_mid = "#6c9066",
  selection = "#536251", -- selected text: sage at 30% over the panel
}

vim.o.background = "dark"
local palette = hues.make_palette({
  background = editor.window,
  foreground = editor.text,
  saturation = "medium",
  n_hues = 8,
  accent = "green",
})
palette.accent = editor.sage
palette.accent_bg = editor.sage_dim
-- Only mini.nvim's groups among the plugin integrations: it is all the engine
-- config ships. Not auto-adjusted, so the overrides below stay.
hues.apply_palette(palette, { default = false, ["nvim-mini/mini.nvim"] = true }, { autoadjust = false })

local function hi(name, attributes)
  vim.api.nvim_set_hl(0, name, attributes)
end

-- Code. The panel draws no bold, which mini.hues marks keywords with: they
-- take the editor's sage instead, strings move to a warm colour away from it,
-- and brackets quieten to the editor's disabled grey.
hi("Statement", { fg = editor.sage, bold = true })
hi("@keyword.return", { link = "Statement" })
hi("Type", { fg = palette.cyan })
hi("String", { fg = palette.orange })
hi("Delimiter", { fg = editor.disabled })

-- The editing area and floating windows: the editor's panel and popup colours.
hi("Normal", { fg = editor.text, bg = editor.window })
hi("NormalFloat", { fg = editor.text, bg = editor.popup })
hi("FloatBorder", { fg = editor.sage_mid, bg = editor.popup })
hi("FloatTitle", { fg = editor.sage, bg = editor.popup, bold = true })
hi("FloatFooter", { fg = editor.disabled, bg = editor.popup })

-- Where you are, and what's selected or found.
hi("CursorLine", { bg = editor.popup })
hi("CursorColumn", { bg = editor.popup })
hi("ColorColumn", { bg = editor.popup })
hi("LineNr", { fg = palette.bg_mid2 })
hi("CursorLineNr", { fg = editor.sage, bg = editor.popup, bold = true })
hi("Visual", { bg = editor.selection })
hi("Search", { fg = editor.text, bg = editor.sage_dim })
hi("IncSearch", { fg = editor.window, bg = editor.sage })
hi("CurSearch", { link = "IncSearch" })
hi("MatchParen", { fg = editor.sage, bg = editor.field, bold = true })

-- Bars: the editor's title and menu bar colour; separators its lines.
hi("StatusLine", { fg = editor.text, bg = editor.deep })
hi("StatusLineNC", { fg = editor.disabled, bg = editor.deep })
hi("WinBar", { fg = editor.text, bg = editor.window, bold = true })
hi("WinBarNC", { fg = editor.disabled, bg = editor.window })
hi("TabLine", { fg = editor.disabled, bg = editor.deep })
hi("TabLineFill", { bg = editor.deep })
hi("TabLineSel", { fg = editor.text, bg = editor.window, bold = true })
hi("WinSeparator", { fg = editor.line })
hi("VertSplit", { link = "WinSeparator" })

-- The completion menu: a popup, its selection the editor's.
hi("Pmenu", { fg = editor.text, bg = editor.popup })
hi("PmenuSel", { fg = editor.text, bg = editor.sage_dim })
hi("PmenuMatch", { fg = editor.sage, bg = editor.popup, bold = true })
hi("PmenuMatchSel", { fg = editor.text, bg = editor.sage_dim, bold = true })
hi("PmenuSbar", { bg = editor.popup })
hi("PmenuThumb", { bg = editor.line })
hi("PmenuBorder", { link = "FloatBorder" })

-- mini.pick, mini.files and mini.clue: popups like the rest, the current
-- line selected the editor's way.
hi("MiniPickMatchCurrent", { fg = editor.text, bg = editor.sage_dim })
hi("MiniPickMatchRanges", { fg = editor.sage, bold = true })
hi("MiniPickPrompt", { fg = editor.text, bg = editor.popup })
hi("MiniPickPromptCaret", { fg = editor.sage, bg = editor.popup })
hi("MiniPickPromptPrefix", { fg = editor.sage, bg = editor.popup })
hi("MiniPickHeader", { fg = editor.sage, bold = true })
hi("MiniPickBorderText", { link = "FloatTitle" })
hi("MiniFilesCursorLine", { fg = editor.text, bg = editor.sage_dim })
hi("MiniFilesTitle", { fg = editor.disabled, bg = editor.popup })
hi("MiniFilesTitleFocused", { fg = editor.sage, bg = editor.popup, bold = true })
hi("MiniClueTitle", { link = "FloatTitle" })
hi("MiniClueSeparator", { fg = editor.line, bg = editor.popup })

vim.g.colors_name = "sfs"

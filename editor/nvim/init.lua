-- SfsEditor's Neovim config: what the editor's Code panel starts Neovim with
-- when you don't have a config of your own (or choose "Engine config").
-- Small: no plugin manager. Besides what ships with Neovim (0.11+), clangd
-- when it is installed and sfs-lsp (the engine's checks of SFS code), it loads
-- modules of mini.nvim that ship beside it (pack/sfs/opt/mini.nvim, mini.nvim
-- v0.18.0, MIT): mini.clue (the keys that can follow a prefix, as you type
-- it), mini.pick (finding files and text), mini.files (a file explorer),
-- mini.icons (file-type icons, when the panel's font draws them) and mini.hues
-- (which makes the "sfs" colour scheme in colors/sfs.lua, the editor's own
-- colours); and grug-far.nvim (pack/sfs/opt/grug-far.nvim, 1.6.80, MIT): find
-- and replace across files, with ripgrep.
--
-- The editor sets, before this runs:
--   g:sfs_editor                1 (also visible to your own config)
--   g:sfs_nvim_mode             "auto" | "engine"
--   g:sfs_project_root          the project folder
--   g:sfs_icons                 1 if the panel's font draws Nerd Font icons

-- Finding files and searching text look in the project's scripts/ folder (the
-- code you write), not the whole project (assets, builds). For the pickers a
-- config of your own brings (Telescope, or LazyVim's), on its usual keys,
-- set once it has finished setting up. Turn it off with
-- `vim.g.sfs_scripts_pickers = false` in your config.
local function scripts()
  local root = vim.g.sfs_project_root
  if not root or root == "" then
    return nil
  end
  local folder = root .. "/scripts"
  return vim.uv.fs_stat(folder) and folder or root
end

local function scope_pickers_to_scripts()
  local function find_files()
    local ok, telescope = pcall(require, "telescope.builtin")
    if ok then
      telescope.find_files({ cwd = scripts(), prompt_title = "Find in scripts" })
    elseif _G.Snacks and Snacks.picker then
      Snacks.picker.files({ cwd = scripts() })
    end
  end
  local function grep()
    -- LazyVim's own picker first (what its grep key used), then Snacks, then
    -- Telescope.
    if _G.LazyVim and LazyVim.pick then
      LazyVim.pick("live_grep", { cwd = scripts() })()
    elseif _G.Snacks and Snacks.picker then
      Snacks.picker.grep({ cwd = scripts() })
    else
      local ok, telescope = pcall(require, "telescope.builtin")
      if ok then
        telescope.live_grep({ cwd = scripts(), prompt_title = "Grep in scripts" })
      end
    end
  end
  local function apply()
    if vim.g.sfs_scripts_pickers == false then
      return
    end
    local map = vim.keymap.set
    map("n", "<leader>ff", find_files, { desc = "Find files (scripts)" })
    map("n", "<leader><space>", find_files, { desc = "Find files (scripts)" })
    map("n", "<leader>sg", grep, { desc = "Grep (scripts)" })
    map("n", "<leader>/", grep, { desc = "Grep (scripts)" })
  end
  -- LazyVim (lazy.nvim) sets its keys on VeryLazy: after those, so these win.
  if package.loaded["lazy"] then
    vim.api.nvim_create_autocmd("User", { pattern = "VeryLazy", once = true, callback = apply })
  else
    vim.api.nvim_create_autocmd("VimEnter", { once = true, callback = apply })
  end
end

-- This folder: the engine config, with its hooks, colour scheme and plugins.
local config_dir = vim.fs.dirname(debug.getinfo(1, "S").source:sub(2))

-- Saving a script tells the editor, and which one is being edited (see
-- sfs_hooks.lua, which "Your config" gets too).
dofile(config_dir .. "/sfs_hooks.lua")

-- "Auto": defer to the user's own config when there is one. Their config can
-- check vim.g.sfs_editor to slim itself down inside the editor.
if vim.g.sfs_nvim_mode == "auto" then
  local user_init = vim.fn.stdpath("config") .. "/init.lua"
  local user_vimrc = vim.fn.stdpath("config") .. "/init.vim"
  if vim.uv.fs_stat(user_init) then
    vim.env.MYVIMRC = user_init
    dofile(user_init)
    scope_pickers_to_scripts()
    return
  elseif vim.uv.fs_stat(user_vimrc) then
    vim.env.MYVIMRC = user_vimrc
    vim.cmd.source(vim.fn.fnameescape(user_vimrc))
    scope_pickers_to_scripts()
    return
  end
end

if vim.g.sfs_project_root and vim.g.sfs_project_root ~= "" then
  vim.fn.chdir(vim.g.sfs_project_root)
end

-- Options ----------------------------------------------------------------------

local opt = vim.opt
opt.number = true
opt.signcolumn = "yes"
opt.cursorline = true
opt.termguicolors = true
opt.mouse = "a"
opt.clipboard = "unnamedplus"
opt.expandtab = true -- the engine's style: 2-space indents
opt.shiftwidth = 2
opt.tabstop = 2
opt.smartindent = true
opt.ignorecase = true
opt.smartcase = true
opt.scrolloff = 4
opt.splitright = true
opt.splitbelow = true
opt.undofile = true
opt.updatetime = 250
opt.completeopt = { "menuone", "noselect", "popup" }
-- The panel's font is plain ASCII: keep UI characters inside it.
opt.fillchars = { eob = " ", vert = "|", fold = "-" }
opt.listchars = { tab = "> ", trail = "-", nbsp = "+" }
opt.title = true -- the panel shows the title (the current file)
opt.titlestring = "%f%m"

-- mini.nvim and the colour scheme are loaded from this folder: the editor
-- starts Neovim with -u, so it isn't on 'runtimepath' or 'packpath' by itself.
opt.runtimepath:prepend(config_dir)
opt.packpath:prepend(config_dir)
vim.cmd.packadd("mini.nvim")

if not pcall(vim.cmd.colorscheme, "sfs") then
  vim.cmd.colorscheme("habamax")
end

-- Keys -------------------------------------------------------------------------

vim.g.mapleader = " "
local map = vim.keymap.set
map({ "n", "i", "v" }, "<C-s>", "<Cmd>write<CR><Esc>", { desc = "Save" })
map("n", "<Esc>", "<Cmd>nohlsearch<CR>")
map("n", "<leader>d", vim.diagnostic.open_float, { desc = "Show diagnostic" })
map("n", "<leader>h", "<Cmd>LspClangdSwitchSourceHeader<CR>", { desc = "Switch header/source" })

-- mini.nvim: key hints, pickers and the file explorer --------------------------

-- Floating windows (pickers, hints, hover) in the panel's plain characters.
opt.winborder = "+,-,+,|,+,-,+,|"

-- File-type icons need a Nerd Font's glyphs, which the editor says the panel's
-- font has (it ships them); without them, plain ASCII. The shipped mini.icons
-- has only glyphs the panel can draw (its Material Design icons, past U+FFFF,
-- are taken out: scripts/stripMiniIcons.py), so the common file types name
-- their Nerd Font icons here; anything else gets a plain file or folder.
local icons = vim.g.sfs_icons == 1
if icons then
  local function icon(glyph, hl)
    return { glyph = glyph, hl = hl }
  end
  local cpp = icon("\u{e61d}", "MiniIconsAzure")
  local image = icon("\u{e60d}", "MiniIconsPurple")
  local shell = icon("\u{e691}", "MiniIconsGrey")
  local shader = icon("\u{eae9}", "MiniIconsCyan")
  local archive = icon("\u{e6aa}", "MiniIconsGrey")
  local font = icon("\u{e659}", "MiniIconsRed")
  require("mini.icons").setup({
    style = "glyph",
    extension = {
      c = icon("\u{e61e}", "MiniIconsBlue"),
      cpp = cpp, cc = cpp, cxx = cpp, inl = cpp, h = cpp, hpp = cpp, hh = cpp, hxx = cpp,
      lua = icon("\u{e620}", "MiniIconsAzure"),
      vim = icon("\u{e62b}", "MiniIconsGreen"),
      py = icon("\u{e606}", "MiniIconsYellow"),
      sh = shell, bash = shell, zsh = shell,
      json = icon("\u{e80b}", "MiniIconsYellow"),
      yaml = icon("\u{e8eb}", "MiniIconsPurple"), yml = icon("\u{e8eb}", "MiniIconsPurple"),
      toml = icon("\u{e6b2}", "MiniIconsOrange"),
      xml = icon("\u{e619}", "MiniIconsOrange"),
      html = icon("\u{e60e}", "MiniIconsOrange"),
      css = icon("\u{e614}", "MiniIconsAzure"),
      js = icon("\u{e60c}", "MiniIconsYellow"),
      cmake = icon("\u{e794}", "MiniIconsGrey"),
      md = icon("\u{e609}", "MiniIconsGrey"),
      txt = icon("\u{e64e}", "MiniIconsGrey"),
      png = image, jpg = image, jpeg = image, gif = image, bmp = image, tga = image, hdr = image, ktx2 = image,
      svg = icon("\u{e698}", "MiniIconsOrange"),
      ttf = font, otf = font, woff2 = font,
      zip = archive, gz = archive, tar = archive, ["7z"] = archive,
      wgsl = shader, glsl = shader, hlsl = shader,
      lock = icon("\u{e672}", "MiniIconsGrey"),
    },
    file = {
      ["CMakeLists.txt"] = icon("\u{e794}", "MiniIconsGrey"),
      [".gitignore"] = icon("\u{e65d}", "MiniIconsOrange"),
      [".gitattributes"] = icon("\u{e65d}", "MiniIconsOrange"),
      [".gitmodules"] = icon("\u{e65d}", "MiniIconsOrange"),
      Makefile = icon("\u{e673}", "MiniIconsGrey"),
      LICENSE = icon("\u{e60a}", "MiniIconsCyan"),
    },
  })
end

local pick = require("mini.pick")
pick.setup({
  window = { prompt_caret = "|", prompt_prefix = "> " },
  -- Without icons, every picker shows plain names (with them, the default).
  source = {
    show = not icons and function(buf, items, query)
      pick.default_show(buf, items, query, { show_icons = false })
    end or nil,
  },
})

local function find_files()
  pick.builtin.files(nil, { source = { cwd = scripts() } })
end
local function grep()
  -- Live grep needs ripgrep; without it, ask for the pattern first.
  if vim.fn.executable("rg") == 1 then
    pick.builtin.grep_live(nil, { source = { cwd = scripts() } })
  else
    pick.builtin.grep(nil, { source = { cwd = scripts() } })
  end
end

map("n", "<leader><space>", find_files, { desc = "Find files (scripts)" })
map("n", "<leader>ff", find_files, { desc = "Find files (scripts)" })
map("n", "<leader>/", grep, { desc = "Grep (scripts)" })
map("n", "<leader>sg", grep, { desc = "Grep (scripts)" })
map("n", "<leader>,", pick.builtin.buffers, { desc = "Buffers" })
map("n", "<leader>fb", pick.builtin.buffers, { desc = "Buffers" })
map("n", "<leader>sh", pick.builtin.help, { desc = "Help pages" })
map("n", "<leader>sR", pick.builtin.resume, { desc = "Resume last picker" })

local files = require("mini.files")
files.setup({
  -- With icons, mini.files' own prefix (from mini.icons); without, a "+" for
  -- folders.
  content = {
    prefix = not icons and function(entry)
      if entry.fs_type == "directory" then
        return "+ ", "MiniFilesDirectory"
      end
      return "  ", "MiniFilesFile"
    end or nil,
  },
  windows = { preview = true },
})
-- Enter opens a file (closing the explorer) or goes into a folder: mini.files
-- maps only l/L for it.
vim.api.nvim_create_autocmd("User", {
  pattern = "MiniFilesBufferCreate",
  callback = function(args)
    map("n", "<CR>", function()
      files.go_in({ close_on_file = true })
    end, { buffer = args.data.buf_id, desc = "Open file or folder" })
  end,
})
map("n", "<leader>e", function()
  -- At the current file, or the scripts folder.
  local file = vim.api.nvim_buf_get_name(0)
  files.open(file ~= "" and vim.uv.fs_stat(file) and file or scripts())
end, { desc = "File explorer" })

-- grug-far.nvim: find and replace across files, ripgrep under the hood. Its
-- icons past U+FFFF (Material Design) get Nerd Font ones the panel draws, or
-- plain characters without icons.
vim.cmd.packadd("grug-far.nvim")
local grug = require("grug-far")
grug.setup({
  transient = true,
  spinnerStates = { "|", "/", "-", "\\" },
  icons = {
    enabled = icons,
    fileIconsProvider = icons and "mini.icons" or false,
    flagsInput = "\u{f024} ",
    resultsStatusReady = "\u{f111} ",
    resultsStatusSuccess = "\u{f058} ",
    resultsEngineLeft = "[",
    resultsEngineRight = "]",
    helpTitle = " \u{f059}  ",
  },
})
map("n", "<leader>sr", function()
  grug.open({ prefills = { paths = scripts() } })
end, { desc = "Search and replace (scripts)" })
map("x", "<leader>sr", function()
  grug.with_visual_selection({ prefills = { paths = scripts() } })
end, { desc = "Search and replace the selection (scripts)" })
map("n", "<leader>sb", function()
  grug.open({ prefills = { paths = vim.fn.expand("%") } })
end, { desc = "Search and replace (this file)" })

-- What can follow a prefix (Space, g, z, [, ], marks, registers, Ctrl-w),
-- shown after a short pause.
local clue = require("mini.clue")
clue.setup({
  triggers = {
    { mode = "n", keys = "<Leader>" },
    { mode = "x", keys = "<Leader>" },
    { mode = "n", keys = "g" },
    { mode = "x", keys = "g" },
    { mode = "n", keys = "z" },
    { mode = "x", keys = "z" },
    { mode = "n", keys = "[" },
    { mode = "n", keys = "]" },
    { mode = "n", keys = "'" },
    { mode = "n", keys = "`" },
    { mode = "n", keys = '"' },
    { mode = "x", keys = '"' },
    { mode = "i", keys = "<C-r>" },
    { mode = "c", keys = "<C-r>" },
    { mode = "n", keys = "<C-w>" },
  },
  clues = {
    { mode = "n", keys = "<Leader>c", desc = "+Code" },
    { mode = "x", keys = "<Leader>c", desc = "+Code" },
    { mode = "n", keys = "<Leader>f", desc = "+Find" },
    { mode = "n", keys = "<Leader>s", desc = "+Search" },
    { mode = "x", keys = "<Leader>s", desc = "+Search" },
    clue.gen_clues.builtin_completion(),
    clue.gen_clues.g(),
    clue.gen_clues.marks(),
    clue.gen_clues.registers(),
    clue.gen_clues.windows(),
    clue.gen_clues.z(),
  },
  window = { delay = 300, config = { width = "auto" } },
})

-- C++: clangd and sfs-lsp ------------------------------------------------------
-- Go to definition (gd / Ctrl-]), hover (K), rename (grn), code actions (gra),
-- references (grr) and completion come from Neovim's built-in LSP client.

if vim.fn.executable("clangd") == 1 then
  -- The project's .clangd points clangd at the game module's compile database.
  vim.lsp.config("clangd", {
    cmd = { "clangd", "--header-insertion=never", "-j=2" },
    filetypes = { "c", "cpp" },
    root_markers = { "project.sfsproj", ".clangd", ".git" },
  })
  vim.lsp.enable("clangd")

  vim.api.nvim_create_user_command("LspClangdSwitchSourceHeader", function()
    local client = vim.lsp.get_clients({ bufnr = 0, name = "clangd" })[1]
    if not client then
      return
    end
    client:request("textDocument/switchSourceHeader", vim.lsp.util.make_text_document_params(0),
      function(_, uri)
        if uri then
          vim.cmd.edit(vim.uri_to_fname(uri))
        end
      end)
  end, {})
end

-- sfs-lsp: [[sfs::...]] completion and checks, and SFS API lints, beside clangd.
-- Inside SfsEditor, SFS_LSP names the sfs-lsp matching the engine; elsewhere it's found on PATH.
local sfs_lsp = vim.env.SFS_LSP or "sfs-lsp"
if vim.fn.executable(sfs_lsp) == 1 then
  vim.lsp.config("sfs_lsp", {
    cmd = { sfs_lsp },
    filetypes = { "c", "cpp" },
    root_dir = function(bufnr, on_dir)
      -- An SFS project, or else the SFS source tree.
      local root = vim.fs.root(bufnr, { "project.sfsproj", ".sfs-engine" })
      if root then
        on_dir(root)
      end
    end,
  })
  vim.lsp.enable("sfs_lsp")
end

-- Completion as you type, and gd, from whichever servers attach (clangd,
-- sfs-lsp): Neovim merges their items.
vim.api.nvim_create_autocmd("LspAttach", {
  callback = function(args)
    local client = vim.lsp.get_client_by_id(args.data.client_id)
    if client and client:supports_method("textDocument/completion") then
      vim.lsp.completion.enable(true, client.id, args.buf, { autotrigger = true })
    end
    map("n", "gd", vim.lsp.buf.definition, { buffer = args.buf, desc = "Go to definition" })
    -- Neovim's grn and gra, under Space c too: a symbol renamed everywhere
    -- clangd knows it, and the servers' fixes (sfs-lsp's lints among them).
    map("n", "<leader>cr", vim.lsp.buf.rename, { buffer = args.buf, desc = "Rename symbol" })
    map({ "n", "x" }, "<leader>ca", vim.lsp.buf.code_action, { buffer = args.buf, desc = "Code action" })
  end,
})

vim.diagnostic.config({ virtual_text = true, severity_sort = true })

-- Highlight yanked text briefly.
vim.api.nvim_create_autocmd("TextYankPost", {
  callback = function() vim.hl.on_yank() end,
})

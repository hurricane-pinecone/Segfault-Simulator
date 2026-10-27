-- SfsEditor's Neovim config: what the editor's Code panel starts Neovim with
-- when you don't have a config of your own (or choose "Engine config").
-- Deliberately small: no plugin manager, no plugins; only what ships with
-- Neovim (0.11+), plus clangd when it is installed.
--
-- The editor sets, before this runs:
--   g:sfs_editor                1 (also visible to your own config)
--   g:sfs_nvim_mode             "auto" | "engine"
--   g:sfs_project_root          the project folder

-- "Auto": defer to the user's own config when there is one. Their config can
-- check vim.g.sfs_editor to slim itself down inside the editor.
if vim.g.sfs_nvim_mode == "auto" then
  local user_init = vim.fn.stdpath("config") .. "/init.lua"
  local user_vimrc = vim.fn.stdpath("config") .. "/init.vim"
  if vim.uv.fs_stat(user_init) then
    vim.env.MYVIMRC = user_init
    dofile(user_init)
    return
  elseif vim.uv.fs_stat(user_vimrc) then
    vim.env.MYVIMRC = user_vimrc
    vim.cmd.source(vim.fn.fnameescape(user_vimrc))
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

vim.cmd.colorscheme("habamax")

-- Keys -------------------------------------------------------------------------

vim.g.mapleader = " "
local map = vim.keymap.set
map({ "n", "i", "v" }, "<C-s>", "<Cmd>write<CR><Esc>", { desc = "Save" })
map("n", "<Esc>", "<Cmd>nohlsearch<CR>")
map("n", "<leader>e", "<Cmd>Explore<CR>", { desc = "File explorer" })
map("n", "<leader>d", vim.diagnostic.open_float, { desc = "Show diagnostic" })
map("n", "<leader>h", "<Cmd>LspClangdSwitchSourceHeader<CR>", { desc = "Switch header/source" })

-- C++: clangd ------------------------------------------------------------------
-- Go to definition (gd / Ctrl-]), hover (K), rename (grn), code actions (gra),
-- references (grr) and completion come from Neovim's built-in LSP client.

if vim.fn.executable("clangd") == 1 then
  -- The project's .clangd points clangd at the game module's compile database.
  vim.lsp.config("clangd", {
    cmd = { "clangd", "--header-insertion=never" },
    filetypes = { "c", "cpp" },
    root_markers = { "project.sfsproj", ".clangd", ".git" },
  })
  vim.lsp.enable("clangd")

  vim.api.nvim_create_autocmd("LspAttach", {
    callback = function(args)
      local client = vim.lsp.get_client_by_id(args.data.client_id)
      if client and client:supports_method("textDocument/completion") then
        vim.lsp.completion.enable(true, client.id, args.buf, { autotrigger = true })
      end
      map("n", "gd", vim.lsp.buf.definition, { buffer = args.buf, desc = "Go to definition" })
    end,
  })

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

vim.diagnostic.config({ virtual_text = true, severity_sort = true })

-- Highlight yanked text briefly.
vim.api.nvim_create_autocmd("TextYankPost", {
  callback = function() vim.hl.on_yank() end,
})

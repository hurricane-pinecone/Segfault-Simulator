-- SfsEditor's hooks, loaded whatever the Code panel's Neovim config: by the
-- engine's init.lua, or before your own config ("Your config"). Loading it
-- again replaces them.
--
-- Saving a file in the project's scripts/ tells the editor, which rebuilds the
-- game (and carries saved values over to renamed fields). Which script is
-- being edited is told too (the UI Preview follows it).
local function report_script_saves()
  local function script_of(name)
    local root = vim.g.sfs_project_root
    if not root or root == "" or name == "" then
      return nil
    end
    local file = vim.fs.normalize(vim.fn.fnamemodify(name, ":p"))
    local scripts = vim.fs.normalize(root .. "/scripts") .. "/"
    if file:sub(1, #scripts) == scripts then
      return file
    end
    return nil
  end
  local group = vim.api.nvim_create_augroup("sfs_script_saves", { clear = true })
  vim.api.nvim_create_autocmd("BufWritePost", {
    group = group,
    callback = function(args)
      local file = script_of(args.file)
      if file then
        vim.rpcnotify(0, "sfs_script_saved", file)
      end
    end,
  })
  vim.api.nvim_create_autocmd("BufEnter", {
    group = group,
    callback = function(args)
      local file = script_of(args.file)
      if file then
        vim.rpcnotify(0, "sfs_script_focused", file)
      end
    end,
  })
end
report_script_saves()

-- SfsEditor's hooks, loaded whatever the Code panel's Neovim config: by the
-- engine's init.lua, or before your own config ("Your config"). Loading it
-- again replaces them.
--
-- Saving a file in the project's scripts/ tells the editor, which rebuilds the
-- game (and carries saved values over to renamed fields). Which script is
-- being edited is told too (the UI Preview follows it), and which are open.
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
  -- Which scripts are open (listed buffers) is told whenever it changes (the
  -- UI Preview goes back to the tab it covered once the last UI file's
  -- closed). Checked once the change has settled: a buffer being deleted is
  -- still listed while its autocommands run.
  local open_reported = nil
  local function report_open_scripts()
    local files = {}
    for _, buf in ipairs(vim.api.nvim_list_bufs()) do
      if vim.bo[buf].buflisted then
        local file = script_of(vim.api.nvim_buf_get_name(buf))
        if file then
          table.insert(files, file)
        end
      end
    end
    local key = table.concat(files, "\n")
    if key ~= open_reported then
      open_reported = key
      vim.rpcnotify(0, "sfs_scripts_open", files)
    end
  end
  vim.api.nvim_create_autocmd({ "BufAdd", "BufEnter", "BufDelete", "BufWipeout", "BufFilePost" }, {
    group = group,
    callback = function()
      vim.schedule(report_open_scripts)
    end,
  })
end
report_script_saves()

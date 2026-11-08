-- 离线验证 LuCI 视图模板能否被 LuCI 自己的编译器接受。
-- 用法：lua verify-luci-view.lua <模板绝对路径>
-- 依据 luci/template.lua 的实际调用方式：tparser.parse(file) -> template, _, err
local path = arg[1]
package.path = "/usr/lib/lua/?.lua;/usr/lib/lua/?/init.lua;" .. package.path
local parser = require "luci.template.parser"

local tpl, _, err = parser.parse(path)
if tpl then
    print("  COMPILE_OK  " .. path)
else
    print("  COMPILE_FAIL " .. tostring(err))
    os.exit(1)
end

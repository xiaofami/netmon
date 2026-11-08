-- 最小 mock：只桩掉 entry/firstchild/template/cbi/_ ，执行 controller index()，
-- 打印它实际注册的路径与标题，用来确认菜单挂在哪个一级菜单下。
local out = {}
local function join(t) return table.concat(t, "/") end

_G.entry = function(path, target, title, order)
    local node = { path = path, title = title, order = order, target = target and "set" or "nil" }
    out[#out+1] = node
    return node
end
_G.firstchild = function() return "__firstchild__" end
_G.template   = function(p) return "template:" .. p end
_G.cbi        = function(p) return "cbi:" .. p end
_G._ = function(s) return s end
_G.module = function() end
_G.package = package
_G.translate = function(s) return s end

package.path = "/usr/lib/lua/?.lua;/usr/lib/lua/?/init.lua;" .. package.path
_G.luci = { http = {}, model = {} }

dofile("/usr/lib/lua/luci/controller/netmon.lua")
index()

for _, n in ipairs(out) do
    print(string.format("  path=%-28s title=%-10s order=%s target=%s",
        join(n.path), n.title or "-", tostring(n.order), n.target))
end
print("  共 " .. #out .. " 条")

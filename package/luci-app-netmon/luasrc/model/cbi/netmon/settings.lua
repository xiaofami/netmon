-- LuCI 设置页：只写 /etc/config/netmon，保存后调用 netmon-apply 让配置生效。
-- 依赖 luci-compat 提供的 CBI 框架（ImmortalWrt 25.12 官方仓库有该包）。
--
-- 面板不再嵌在本页（iframe 受 LuCI 主题的高度/宽度限制，桌面端只剩 60vh、
-- 移动端还要横向滚动，观感差），改为在页头描述里给一个直达按钮。
-- 经典 CBI 的 map/section description 是原样输出的（不是 pcdata 转义），
-- 所以这里可以直接放 <a>，不需要额外的 DummyValue 行。
local sys = require "luci.sys"


-- 读一个 UCI 标量，失败一律返回空串。
-- 刻意用 `uci -q get` 而不是 luci.model.uci：新版 LuCI 的 uci:get 走 ucode 桥接，
-- 标量也可能返回 table（实测 network.lan.ipaddr 返回 { "192.168.1.1/24" }），
-- 一取长度或 gsub 就 500。命令行输出的永远是纯字符串。
local function uci_get(opt)
    local ok, out = pcall(function()
        return sys.exec("uci -q get " .. opt .. " 2>/dev/null")
    end)
    if not ok or type(out) ~= "string" then
        return ""
    end
    return (out:gsub("%s+$", ""))
end


-- 拼独立实例的访问地址，直接显示在页头，省得用户去翻端口配置。
-- listen_addr 为 lan 时取 network.lan.ipaddr；其余（127.0.0.1 / 具体 IP / 0.0.0.0）原样用。
local function panel_url()
    local port = uci_get("netmon.main.http_port")
    if port == "" then
        port = "8090"
    end

    local addr = uci_get("netmon.main.listen_addr")
    if addr == "" or addr == "lan" then
        addr = uci_get("network.lan.ipaddr")
    end
    addr = addr:gsub("/.*$", "")
    if addr == "" or addr == "0.0.0.0" then
        addr = "127.0.0.1"
    end

    return "http://" .. addr .. ":" .. port .. "/"
end


m = Map("netmon", translate("Network Monitor 设置"),
    string.format(
        '<div>%s</div>'
        .. '<div style="margin-top:12px">'
        .. '<a class="cbi-button cbi-button-apply" href="/netmon/" target="_blank" '
        .. 'rel="noopener">%s</a>'
        .. '<span style="margin-left:10px;opacity:.7">%s</span>'
        .. '</div>',
        translate("只读旁路监控，不影响上网。保存后会自动重载面板服务并重启采集进程。"),
        translate("打开监控面板"),
        translatef("独立端口访问：%s", panel_url())))


s = m:section(NamedSection, "main", "netmon", translate("基本设置"))
s.addremove = false

o = s:option(Flag, "enabled", translate("启用监控"))
o.default = "1"
o.rmempty = false

o = s:option(Value, "http_port", translate("面板端口"),
    translate("独立 Web 实例的监听端口；保存后自动重载，浏览器需改用新端口访问。"))
o.datatype = "port"
o.default = "8090"
o.rmempty = false

o = s:option(Value, "listen_addr", translate("监听地址"),
    translate("lan 表示自动使用 LAN 地址（推荐）。也可填具体 IP 或 127.0.0.1；除非已配置防火墙，请勿填 0.0.0.0，面板没有鉴权。"))
o.default = "lan"
o.rmempty = false

o = s:option(Value, "db_path", translate("数据库位置"),
    translate("需为可写的持久化路径。修改后新数据写入新位置，已有历史数据不会自动迁移；请勿使用 /tmp。"))
o.default = "/opt/netmon/data/netmon.db"
o.rmempty = false

o = s:option(ListValue, "retain", translate("明细保留天数"),
    translate("超期的连接与 DNS 明细会被清理，小时/日聚合长期保留。Flash 较小的路由器建议 3–7 天。"))
for _, d in ipairs({1, 3, 7, 14, 30, 60, 90}) do
    o:value(tostring(d), tostring(d) .. " " .. translate("天"))
end
o.default = "30"
o.rmempty = false

o = s:option(ListValue, "interval", translate("采样间隔"),
    translate("读取 conntrack 的频率。5 秒足够细，调小会增加数据量与数据库体积。"))
o:value("3", "3 " .. translate("秒"))
o:value("5", "5 " .. translate("秒（推荐）"))
o:value("10", "10 " .. translate("秒"))
o:value("30", "30 " .. translate("秒"))
o.default = "5"
o.rmempty = false

o = s:option(Flag, "loglevel", translate("调试日志"),
    translate("仅在排障时开启，日志量较大。"))
o.default = "0"
o.rmempty = false

-- 后台执行：先让保存页面返回，再重载服务，避免 uhttpd reload 卡住请求。
function m.on_after_commit(self)
    sys.call("(sleep 1; /usr/bin/netmon-apply >/dev/null 2>&1) &")
end

return m

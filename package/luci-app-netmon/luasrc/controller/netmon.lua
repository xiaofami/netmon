-- LuCI 菜单：挂在「状态」下，名称「监控面板」，点开直达「设置」页。
--
-- 为什么不再内嵌面板
-- ------------------
-- iframe 嵌在 LuCI 里有两层高度约束（LuCI 内容区 + 主题侧栏），桌面端只剩 60vh
-- 左右，图表被压扁、表格要横向滚动，移动端更糟；而 LuCI 主题的宽度上限也把
-- 面板的多列布局挤成单列。面板本身是独立单页应用，按自己的视口做响应式才好看，
-- 所以这里只保留设置页，面板通过「打开监控面板」链接/独立端口访问：
--
--   * 同源路径   http://<路由器地址>/netmon/            （uhttpd 主实例直接给，无需额外端口）
--   * 独立端口   http://<LAN 地址>:<http_port>/         （UCI 里配置，默认 8090）
module("luci.controller.netmon", package.seeall)

function index()
    -- 单个 entry 直接指向 CBI 设置页：没有子节点，所以不会出现一排只有一项的 tab。
    entry({"admin", "status", "netmon"}, cbi("netmon/settings"),
          _("监控面板"), 60).leaf = true
end

# Web 可视化前端

访问地址：**http://192.168.1.1:8090/**

---

## 架构

```
浏览器
  │ GET /
  ├──────────────► uhttpd (netmon 实例, 192.168.1.1:8090)
  │                  home = /www/netmon
  │                  └─ index.html  (单文件，纯原生 JS)
  │
  │ GET /cgi-bin/data?since=86400
  └──────────────► /www/netmon/cgi-bin/data  (sh 脚本)
                     └─ exec netmon-report dashboard --since 24h
                          └─ 只读打开 SQLite，输出完整 JSON
```

**一个请求拿全部数据。** 路由器上每次 CGI 都要 fork 进程，
拆成多个接口会明显变慢。所以 `dashboard` 子命令一次性输出
概况 + 流量排行 + 趋势 + 探测 + DNS。

---

## 为什么不用现成方案

| 方案 | 问题 |
|---|---|
| Grafana / Prometheus | 要额外装 100MB+ 组件，路由器上不合适 |
| LuCI 应用 | 侵入 LuCI，升级固件会被覆盖，且要写 Lua |
| 前端框架（Vue/React） | 要 CDN 或打包，路由器离线场景不可用 |
| **原生 JS 单文件** | ✅ 15KB，无依赖，uhttpd 直接托管 |

前端**不引用任何外部资源**（无 CDN、无字体、无框架）。
断网时页面照常工作 —— 这对一个网络监控工具是硬要求。

---

## 部署

### 1. 文件位置

```
/www/netmon/index.html          页面（单文件，含 CSS + JS）
/www/netmon/cgi-bin/data        CGI 接口（sh，755）
```

`/www` 在 overlay 上，**重启不丢**，但**升级固件会被覆盖**。
升级后重新拷一次即可（见 `DEPLOY.md`）。

### 2. uhttpd 独立实例

```bash
uci set uhttpd.netmon=uhttpd
uci set uhttpd.netmon.home='/www/netmon'
uci set uhttpd.netmon.listen_http='192.168.1.1:8090'
uci set uhttpd.netmon.cgi_prefix='/cgi-bin'
uci set uhttpd.netmon.index_page='index.html'
uci set uhttpd.netmon.max_requests='10'
uci set uhttpd.netmon.max_connections='30'
uci set uhttpd.netmon.script_timeout='20'
uci set uhttpd.netmon.network_timeout='20'
uci set uhttpd.netmon.no_symlinks='1'
uci set uhttpd.netmon.rfc1918_filter='0'
uci commit uhttpd
/etc/init.d/uhttpd reload
```

### 3. 关键安全点：只绑 LAN IP

```
listen_http='192.168.1.1:8090'      ← 只绑局域网地址
```

**不是 `0.0.0.0:8090`**。这样：
- WAN 侧访问不到（PPPoE 地址上没有这个监听）
- 不经过 caddy，不影响任何现有站点
- 不需要改防火墙（本来就没有暴露）

验证：
```bash
netstat -tlnp | grep 8090
# tcp  0  0 192.168.1.1:8090  0.0.0.0:*  LISTEN  <pid>/uhttpd
```

### 4. 与 LuCI / caddy 的关系

| 服务 | 端口 | 影响 |
|---|---|---|
| LuCI (uhttpd main) | 127.0.0.1:8080 | **不受影响**，PID 不变 |
| caddy | :80 / :443 | **完全不动**，没有改 Caddyfile |
| netmon (uhttpd) | 192.168.1.1:8090 | 新增 |

`/etc/init.d/uhttpd reload` 只重载配置，LuCI 连接不断。

---

## 页面功能

### 顶部指标卡

| 卡片 | 数据来源 |
|---|---|
| 连接明细 | `conns` 总行数 |
| 总流量 | `trend` 里所有桶的上下行求和 |
| 国内带宽 | `probe` 里 `tag=domestic` 的最大 `bw` |
| 出国带宽 | `probe` 里 `tag=oversea` 的最大 `bw` |
| 出国延迟 | `probe` 里 `104.x` 目标的最小 `rtt` |
| 数据库 | `page_count × page_size` |

### 流量趋势

Canvas 手绘，**不用图表库**（省 200KB 依赖）。

- 绿柱 = 下行，蓝柱 = 上行，堆叠显示
- Y 轴自动缩放，标注 4 档
- X 轴最多标 6 个时间点

### 流量排行

- **按网段**：带横向进度条，直观看出占比
- **按域名**：靠 DNS 嗅探反查出来的，没命中的不显示

### 线路质量

每行一个探测目标：延迟 / 丢包 / 带宽。丢包率配色：
- 绿色 < 5%
- 黄色 5~30%
- 红色 > 30%

### DNS 解析

按查询次数排序。耗时配色：
- 绿色 < 10ms（本地缓存）
- 黄色 10~100ms
- 红色 > 100ms

### 时间范围

右上角 1 小时 / 24 小时 / 7 天，切换后重新请求。

### 自动刷新

每 10 秒一次。左上角圆点：绿色=正常，红色=请求失败。

失败时页面顶部显示红色提示条，明确告诉你问题在哪
（接口 500 / JSON 解析失败 / 请求超时），不会白屏。

---

## 修改前端

### 改完怎么部署

```bash
# 本地改 www/index.html 或 www/cgi-bin-data
cd netmon/www
tar cf - index.html cgi-bin-data \
  | ssh -F ~/.ssh/netmon.conf r 'cd /www/netmon && tar xf - && \
      mv cgi-bin-data cgi-bin/data && chmod 755 cgi-bin/data'
```

**不用重启任何服务**，刷新浏览器即可看到。

### 改接口返回的字段

改 `src/netmon-report.c` 的 `cmd_dashboard()`，然后：

```bash
cd src
tar cf - netmon-report.c | ssh -F ~/.ssh/netmon.conf r 'cd /opt/netmon/src && \
  tar xf - && gcc -O2 -Wall -Wextra -o netmon-report netmon-report.c \
  -I/opt/netmon/include -L/opt/netmon/sqa/sqlite-amalgamation-3500300 -lsqlite3 && \
  cp netmon-report /usr/bin/'
```

### 本地预览（不用连路由器）

把一份 `dashboard` 输出存成 JSON，生成内嵌数据的预览页：

```python
preview = open('index.html').read()
preview = preview.replace(
  "function load(){\n  setDot('live');\n  var xhr",
  "function load(){\n  setDot('live');\n"
  "  if(window.__DATA__){ render(window.__DATA__); return; }\n  var xhr")
preview = preview.replace("<script>\n'use strict';",
  "<script>\nwindow.__DATA__ = " + json_text + ";\n'use strict';")
open('preview.html','w').write(preview)
```

---

## 排查

### 页面打不开

```bash
netstat -tlnp | grep 8090          # 有没有在监听
logread | grep uhttpd | tail -20   # uhttpd 有没有报错
ls -la /www/netmon/                # 文件在不在
```

### 页面开了但没数据

```bash
# 直接测 CGI，绕过浏览器
curl "http://192.168.1.1:8090/cgi-bin/data?since=86400" | head -c 300
```

- 返回 JSON → 前端问题，看浏览器 Console
- 返回 `{"error":"query failed"}` → 数据库或 netmon-report 问题
- 返回 `{"error":"netmon-report not found"}` → `/usr/bin/netmon-report` 丢了

### 数字看起来不对

```bash
netmon-report dashboard --since 24h | head -c 500
```

和页面对比。如果命令行对但页面不对，是前端渲染问题。

### CGI 报 500

```bash
# 手动跑一遍，看 stderr
QUERY_STRING="since=86400" sh -x /www/netmon/cgi-bin/data
```

---

## 后续可加的功能

按价值排序：

1. **实时连接列表** —— 当前活跃连接（需要从 conntrack 现读，不是查库）
2. **告警** —— 出国带宽低于阈值、丢包超阈值时页面标红
3. **历史对比** —— 今天 vs 昨天同时段
4. **导出 CSV** —— 报表页加下载按钮
5. **深色/浅色主题切换** —— 现在是纯深色

都不难，但每次改动都要重新验证 LED 那个"不许断网"的底线。

---

## 布局重构：从 `<table>` 换成 CSS Grid（2026-11-06）

### 原来的问题

用户反馈三条：IP 显示不全、字没对齐、不好看。根因都在表格上：

```css
/* 旧代码 */
td{padding:7px 8px 7px 0;white-space:nowrap;
   overflow:hidden;text-overflow:ellipsis;max-width:0}
.k{max-width:220px;overflow:hidden;text-overflow:ellipsis}
```

`max-width:0` 是给 `<table>` 做"可截断弹性列"的经典 hack，
但它让**所有列**都变成可压缩的。设备表有 7 列，
浏览器按内容比例分配后，IP 列（`192.168.1.100`）被压到放不下，
于是显示成 `192.168.1.2…`。

同时 `.k` 的 `max-width:220px` 又给第一列加了额外约束，
和表格自身的宽度计算互相打架 —— 这就是"字没对齐"的来源：
有的列被压窄、有的被 `max-width` 截断，右边界的数字列就对不齐了。

### 现在的做法：grid 行布局

完全不用 `<table>`，每个数据行是一个 `display:grid` 的 div：

```css
.rows .rhead,.rows .r{display:grid;align-items:center;gap:12px;padding:0 15px}
.g-dev{grid-template-columns:minmax(0,1.35fr) 104px 62px 62px 96px minmax(0,1.05fr)}
```

关键点：

1. **`minmax(0, 1fr)` 而不是 `1fr`**
   这是最容易踩的坑。`1fr` 的最小尺寸是 `auto`（内容宽度），
   长域名会把列撑开甚至撑破容器；`minmax(0,1fr)` 把最小尺寸设为 0，
   配合子元素的 `overflow:hidden;text-overflow:ellipsis` 才能正确截断。

2. **IP / 数字列给固定像素宽**
   `104px` 是量出来的：等宽字体 12px 下 `192.168.1.100`（11 字符）
   约 79px，留 25px 余量。固定宽度意味着**永远不参与压缩**，
   不会被别的列挤掉。

3. **数字统一 `.num`**
   `text-align:right` + `font-variant-numeric:tabular-nums`。
   tabular-nums 让所有数字等宽，右对齐后小数点/个位自然成一条线，
   滚动时不会左右跳。

4. **窄屏隐藏次要列而不是压缩**
   ```css
   @media (max-width:820px){
     .g-dev{grid-template-columns:minmax(0,1fr) 62px 90px}
     .g-dev .c-ip,.g-dev .c-hosts,.g-dev .c-top{display:none}
   }
   ```
   表格在窄屏压缩列宽只会让所有东西都看不清。
   直接砍掉次要列（IP、域名数、最常查询），保住设备名和应答率。

### 视觉上的改进（参考 Netdata / Grafana 的做法）

| 元素 | 做法 | 为什么 |
|---|---|---|
| KPI 卡片 | 左侧 2px 色条按状态变色 | 不用读数字，扫一眼颜色就知道好坏 |
| 总流量 | 卡片内嵌 SVG 迷你趋势线 | 数字是"多少"，曲线是"趋势"，两个信息不占两处 |
| 网段排行 | 域名下方 3px 占比条 | 纯数字看不出量级差，条形能 |
| 设备排行 | 首字母彩色徽章 + 查询量占比条 | 靠颜色认设备比读名字快 |
| 应答率/丢包 | 状态徽章（绿/黄/红） | 阈值判断交给机器，不用人算 |
| 表格行 | hover 时淡蓝底 | 长表格里跟踪行 |
| 状态点 | 呼吸动画 | 静止的绿点和"卡住"分不清 |

设备徽章的颜色用名字哈希生成，**同一台设备每次刷新颜色不变**：

```js
function devBadge(name){
  var h = 0;
  for(var i = 0; i < name.length; i++){ h = (h * 31 + name.charCodeAt(i)) & 0xffff; }
  var hue = h % 360;
  // hsla(hue,62%,55%,.16) 背景 + hsl(hue,68%,68%) 文字，暗色下够亮
}
```

### 配色调整

底色从 `#0f1419` 压到 `#0a0d12`，卡片 `#12171f`。
对比度拉开之后，边框可以用更暗的 `#222c38`，视觉上更干净
（原来边框和背景太接近，表格行黏在一起）。

### 踩坑记录

- **`el.closest('.card')`**：`s-lat` 是 `.card > .value`，
  用 `closest` 往上找容器比 `parentNode.parentNode` 稳，
  以后改结构也不会断。
- **`viewBox="0 0 100 22"` + `preserveAspectRatio="none"`**：
  SVG 迷你图这样写能自适应容器宽度，不用 JS 算像素。
- **resize 重绘要防抖**：拖窗口时每像素都重绘 canvas 会卡，
  `setTimeout(120ms)` 包一层。
- **部署后记得 cp 到 `/www/netmon/`**：源码在 `/opt/netmon/www/`，
  uhttpd 的 home 是 `/www/netmon`，两处都要更新（已经踩过两次）。

### 验证方法

改完前端不能只看"页面能打开"，至少验证三层：

```bash
# 1) 页面和接口都通
curl -s -o /dev/null -w "page=%{http_code} " http://192.168.1.1:8090/
curl -s -o /dev/null -w "api=%{http_code}\n" "http://192.168.1.1:8090/cgi-bin/data?since=86400"

# 2) JS 语法（把 <script> 抽出来单独 check）
node --check build/check.js

# 3) DOM id 引用的元素都真的存在
#    getElementById 拿到 null 会在运行时抛错，页面白屏
#    用脚本比对 HTML 里的 id 和 JS 里引用的 id
```

第 3 步最容易出事，也最容易自动化检查 —— 比打开浏览器点一遍可靠。

---

## 2026-11-06 第二次重构：AdGuard Home 式多视图面板

### 需求与目标

用户要求：面板内容更丰富、参照 AdGuard Home 的精致度、**每台设备都能点开查询**、
**每台设备都要显示名称**，没有名字的去路由器静态地址分配处找。

旧版是单页长滚动 + 6 个表格，设备名只读 `/tmp/dhcp.leases`，
静态 IP 设备（NAS-1 等）全部显示「未登记设备」，且点不了。

### 后端：设备身份四层来源（`netmon-report.c`）

| 优先级 | 来源 | 说明 |
|---|---|---|
| 0 | `/etc/config/dhcp` 的 `config host` | LuCI「静态地址分配」，用户手填的名字，最可信。48 台 |
| 1 | `/tmp/dhcp.leases` | dnsmasq 动态租约，覆盖手机/笔记本 |
| 2 | `/proc/net/arp` 按 MAC 反查 | 设备换了 IP 也能认出来。实测 `192.168.1.104` 的 MAC 就是静态分配里的 `Phone-5` |
| 3 | 自动生成 | 用 OUI 厂商 + IP 尾段生成，如 `TP-LINK 设备 · 100` |

关键实现点：

- **静态分配的名字必须优先于租约**：用户手填 > 客户端自报。`dev_add()` 里
  `source < d->source` 才覆盖名字。
- **纯文本解析 `/etc/config/dhcp`，不 fork `uci show`**：CGI 每 10 秒跑一次，
  省一次进程创建，也不依赖 uci 在 PATH 里。
  同时兼容 `option mac` 和 `list mac` 两种写法。
- **ARP 只补身份，不往目录里塞没有 DNS 活动的设备**，避免客户端列表膨胀。
  `flag=0x0`（未确认）的条目 MAC 通常仍是真实的，只排除全零地址，不要整个丢掉。
- 超长的海康摄像头型号（`DS-2SC3Q140MY-TE...`）简化成「监控摄像头 · 239」，
  原始名仍保留在 `name` 字段，`display` 只用于显示。
- OUI 前缀表覆盖本网络现有厂商，前端据此选设备图标和中文类型标签。

结果：**53 台设备全部有名字**（48 静态 + 1 租约 + 1 MAC 反查 + 3 自动）。

### 后端：`client` 子命令

```
netmon-report client --ip 192.168.1.101 --since 24h
```

始终输出 JSON，字段：`device` / `summary` / `timeline` / `top_domains` /
`qtypes` / `rcodes` / `recent`(80 条) / `traffic`。

安全：IP 是外部输入，先过 `valid_ip()` 白名单（只允许 `[0-9a-fA-F.:]` 且必须含
`.` 或 `:`），所有 SQL 一律 `sqlite3_bind_text`，**不拼字符串**。
CGI 侧再做一遍 `tr -cd` 过滤，双保险。

`dashboard` 同步扩展：`summary` 增加 queries / clients / catalog / blocked /
reply_rate / avg_ms / 窗口流量；`devices` 输出**全量设备**（含无活动的），
每台带 `device` 身份对象 + DNS/流量统计；新增 `dns_timeline` 和 `dns_recent`。

性能：设备的 Top 域名改用**窗口函数一次扫完**
（`ROW_NUMBER() OVER (PARTITION BY client_ip ...)`），不再为每台设备跑一次子查询；
conns 也用一条 `GROUP BY src` 汇总，避免 N+1。

### CGI

`/www/netmon/cgi-bin/data` 支持 `view=client&ip=...` 路由到 `client`，
其余走 `dashboard`。`since` 仍是纯数字白名单。

**坑**：后端对非法 IP 会输出 `{"error":...}` 并以非零码退出，
如果 CGI 写成 `cmd || printf '{"error":...}'`，就会拼出**两段 JSON**，
前端 `JSON.parse` 直接失败。改成先接住输出到变量，只有输出为空才补错误对象。

### 前端结构

单文件 SPA，左侧固定侧栏 + 7 个视图，hash 路由（`#/client/192.168.1.101`）：

| 视图 | 内容 |
|---|---|
| 总览 | 6 张 KPI（带迷你趋势线）、DNS 活动柱状图、流量柱状图、活跃设备/域名/线路/最近查询 |
| 客户端 | 53 张设备卡片（或列表），搜索 / 按来源筛选 / 卡片列表切换，**点任意一台进详情** |
| 设备详情 | hero（名称/IP/MAC/厂商/类型/来源）、6 张 KPI、小时活动图、Top 域名、应答环形图、流量目的地、80 条查询日志 |
| 查询日志 | 全局最近 60 条，按 rcode 筛选 + 搜索，客户端名可点进详情 |
| 域名排行 | DNS 次数 / 流量 / 目的网段三个榜 |
| 线路质量 | KPI + 探测目标明细 |
| 系统信息 | 数据库概况、数据来源说明、常用命令 |

设计：深浅双主题（localStorage 记忆）、CSS 变量、13px 圆角卡片、
细边框 + 轻阴影、状态徽章、占比条、hover 微动效、响应式（窄屏侧栏变横向导航）。
全部内联 SVG 图标，无 CDN 依赖。

**图表全部用 DOM/CSS 柱状 + SVG 环形，不用 canvas**：自适应宽度，
不用处理 DPR 和 resize 重绘。

### 验证：四层

```bash
python3 build/check-web.py                 # 1) node --check + DOM id 引用比对
python3 build/make-preview.py              # 2) 抓线上真数据生成离机预览页
node build/render-test.js                  # 3) DOM 桩逐视图执行，断言渲染结果
curl ...                                   # 4) 线上页面 / 两个接口 / 注入用例
```

`render-test.js` 用极简 DOM 桩在 Node 里**真实执行页面脚本**，逐个切视图，
能抓到语法检查抓不到的运行时异常（字段 undefined、某个视图白屏、切换设备残留旧数据）。
同时 `preview-live.html` 内嵌真实数据，通过 `NETMON_DEMO` 钩子让 `xhr` 同步返回，
`file://` 打开即可完整交互，也方便交付演示。

### 本次踩坑

| 坑 | 处理 |
|---|---|
| 路由器 busybox **没有 `install` / `stat`** | `make install` 曾静默失败导致二进制没换；Makefile 改成 `mkdir -p` + `cp` + `chmod` |
| `tar -C a f1 -C b f2` 的 `-C` 是累积的 | 第二个相对路径从第一个目录出发，容易找不到；分两次传或用绝对路径 |
| SVG 的 `stroke="var(--accent)"` 不生效 | presentation attribute 不解析 CSS 变量，必须写 `style="stroke:var(--accent)"` |
| CGI 拼出两段 JSON | 见上文，先接住输出再判断 |
| Firefox headless 在沙箱里起不来（`uid_map: EPERM`） | 放弃截图，改用 Node DOM 桩做运行时验证，覆盖度更高 |
| `make install` 之后要确认进程/文件真的换了 | `ls -la` 看时间戳和大小，别只看命令退出码 |

---

## 2026-11-06 第三次迭代：拦截与代理视图 + 文案口语化

新增第 7 个视图 `#/blocks`「拦截与代理」，把 homeproxy 的拦截规则、
DNS 异常归因和代理可达性建议集中呈现。后端与整体设计见 **`docs/HOMEPROXY.md`**，
这里只记前端相关的部分。

### 新增内容

- 侧栏「分析」组新增「拦截与代理」；总览的「广告 / 规则拦截」KPI 可点击直达。
- 视图内：6 张 KPI、规则版本/同步状态、生效规则清单、被拦截域名榜、
  真实解析失败榜、代理探测结果表（`need_proxy` 行高亮）、规则变更历史、判读说明。
- 查询日志、域名排行、设备详情里的异常项统一带归因徽章：
  `广告拦截` / `本地域` / `解析失败`，替代原来裸的 NXDOMAIN/REFUSED。
- 判定徽章 `clsPill()` / `verdictPill()` / `codeCell()` 复用同一套 `.pill` 样式。

### 界面文案规范（本轮统一整改）

内部术语不准出现在界面上：

| 原表述 | 改成 |
|---|---|
| `25 个小时桶` / `峰值 X / 桶` | `按小时统计 · 共 25 小时` / `最忙一小时 X 次` |
| `窗口总流量` / `窗口内连接` | `总流量` / `所选时段连接` |
| `指纹 xxxx` / `匹配模式数` | `规则版本 xxxx` / `规则条目数` |
| `同目标 24h 冷却` | `同一目标 24 小时内不重复测` |
| `应答率` / `已登记设备` / `静默` | `成功率` / `已知设备` / `暂无活动` |

`build/patch-copy.py`（一次性补丁，已删除）把这类词全部替换，
`build/check-web.py` 会扫描残留术语，防止回潮。

### 验证工具

```
python3 build/check-web.py      # 语法 + DOM id + SVG 变量 + 残留术语
python3 build/make-preview.py   # 抓线上真数据生成离机预览页
node build/render-test.js       # DOM 桩逐视图执行，49 项断言全过
```

`render-test.js` 覆盖新视图的关键渲染、总览 KPI 跳转、切换设备不残留旧数据，
比人工点一遍可靠且可重复。

---

## 2026-11-07 第四次迭代：流量排行、隐私观察、移动端全字段

### 用户反馈与处理

| 反馈 | 处理 |
|---|---|
| 「25 个小时桶 / 峰值 41,089 / 桶」还在 | 排查确认线上文件已是新版，是**浏览器缓存**。加 `no-store` meta + 页脚版本戳；把仅剩两处「峰值」改成「实测最快」；仍需用户强刷一次 |
| 想要设备流量排名、域名/IP 流量排名 | 新增「流量排行」视图：设备榜（可点进详情）、域名/IP 榜、服务大类分布、目的网段 |
| 想判断是什么服务（抖音/快手/下载） | 新增 `src/apps.c`：域名关键词 + 端口 + 上下行比例三层特征；详见 `docs/APPS.md`（含 nDPI / libprotoident 选型结论） |
| 国产手机内置录屏上传，能否监控到 | 查证事件后新增「隐私观察」视图：严格小上行判定 + 原始上行观察 + 遥测域名线索，明确可观测边界与误报风险 |
| 移动端信息显示不全 | 旧方案靠 `display:none` 藏列，改为**卡片式全字段**：`labelizeRows()` 从 `.rhead` 自动提 `data-label`，窄屏每格显示「标签 + 值」，一列不藏 |

### 前端结构变化

- 导航 7 → 9 项；新增 `#/traffic`、`#/privacy` 两个视图。
- 新增通用工具 `appPill()` / `ratioBar()` / `labelizeRows()`；
  `renderView()` 两个分支末尾都会调 `labelizeRows()`（client 分支有提前 return，别漏）。
- 总览「总流量」KPI 可点击直达流量排行。
- 响应式断点：`≤900px` 调整密度，`≤760px` 表格转卡片。
  `.rows .rhead` 在卡片模式下隐藏，但 JS 仍从它读表头文字，不能删 DOM。

### 后端

`netmon-report.c` 新增 `emit_traffic()` / `emit_privacy()`，
dashboard 输出 `traffic_hosts` / `app_stats` / `privacy{suspects,watch,telemetry}`。
`apps.c` 只被报表链接，采集进程不受影响（Makefile 的 `REPORT_SRC`）。

### 审查修正

- `app_stats.pct` 原按「最大单条 × 40」估算，改成按类别总和算，合计恰好 100%。
- `dev_decorate()` 对 IPv6 地址原来会把整串地址塞进兜底名，改为取末段后 12 字符。
- 严格隐私判定保持高阈值（≥40 次短时上行），实测 0 条是真实结论，
  不为了「有结果」放宽到误报；另给 `watch` 原始指标供观察。

---

## 2026-11-07 第五次迭代：通用详情下钻与上下行独立排名

### 用户反馈

> 流量分析应区分上行与下行单独排名。点条目应能列出相关设备、连接时间、
> 对应时间段流量；DNS 查询只列流量最大域名太少；「建议加代理规则」显示 1
> 但不知道是哪个；总览里的 DNS 查询、活跃客户端习惯上都能点开却点不开；
> 隐私观察里 IP 显示不全、出现「设备 · 92e9」这类怪名字。

核心判断：这是**下钻能力的系统性缺口**，不该给每个榜单独打补丁，
而应做一套通用详情机制，再把所有条目接上去。

### 后端：一个接口覆盖五类实体

`netmon-report detail --type T --key K --since 24h`（`src/detail.c`），
T 取 `host` / `qname` / `cidr` / `app` / `verdict`。统一返回：

| 字段 | 内容 |
|---|---|
| `summary` | 连接数、上下行字节、关联设备数、DNS 次数、首末时间、服务判断 |
| `devices` | 按连接 / 查询次数排序的关联设备 |
| `timeline` | 按小时的上行 / 下行字节（qname 为查询次数） |
| `conns` | 最近 60 条连接：时间、设备、目的地、端口、时长、上下行 |
| `dns` | 最近 60 条 DNS：时间、设备、rcode、耗时、应答 |
| `items` | verdict 专用：具体是哪些目标建议加代理规则 |

- **全参数化**：所有键走 `sqlite3_bind_text`，不拼 SQL。
- **两段校验**：CGI 白名单清洗 → `detail_valid()` 允许 `%XX` →
  `url_decode()` → `decoded_key_ok()` 再挡一次 shell/SQL 元字符，
  避免把注入串原样回显到页面上。
- **qname 单独处理**：被拦截的域名根本没有连接记录，所以关联设备、
  时间范围、小时趋势全部改从 `dnsq` 聚合；否则「关联设备 0」看着像坏了。

### CGI 踩坑：`tr -cd` 把 URL 编码的 `%` 吞了

原来写 `tr -cd 'A-Za-z0-9._:-%'`，`tr` 把 `:-%` 当成**字符范围**，
`%` 一起被删掉，中文服务类别（`%E4%B8%8B%E8%BD%BD`）变成
`E4B88BE8BDBD`，点服务榜打不开。改成 `'A-Za-z0-9._:%-'`——
`-` 必须放在字符集最后才是字面量。

### 前端：一套 `data-detail` 走遍所有榜单

- `dt(type,key)` 生成 `data-detail` + `data-dkey`；全局事件委托统一转成
  `#/detail/<type>/<encodeURIComponent(key)>`。
- **优先级**：行内 `data-goto`（点设备名进设备详情）→ `data-navto`（KPI 跳视图）
  → `data-detail`（点行其余位置进通用详情），所以一行上能有两种下钻。
- 新增 `#/detail` 视图：hero + 6 张 KPI + 小时趋势 + 关联设备 +
  连接明细 + DNS 明细 + 代理建议清单；`S.detailFrom` 记住来源，
  左上角「返回」回原页面。切换条目时清空旧数据，不闪现上一项。
- `kpi()` 增加 `o.detail` / `o.dkey`；总览 6 张 KPI 全部可点。
- 覆盖范围：总览（热门域名 / 最近查询 / 线路质量 / KPI）、拦截与代理
  （拦截域名 / 真实异常 / 探测表 / 建议 KPI）、流量排行、域名排行、
  查询日志、设备详情内三个榜、隐私观察遥测线索、线路质量目标。

### 上下行独立排名

「流量排行」新增 4 张独立榜：下载最多的设备 / 上传最多的设备 /
下载最多的域名 IP / 上传最多的域名 IP，各自独立排序，
避免上行被大下载淹没。完整设备榜另加 `data-s` 切换按合计 / 下载 / 上传排序。

### 隐私观察可读性

- 后端 `dev_decorate()`：IPv6 隐私地址末段是随机十六进制，不再拿来当名字
  （`设备 · 92e9`），统一显示「未命名设备（IPv6）」；能查到 MAC 的仍返回真实登记名。
- `privacy` 的 watch / suspects SQL 增加 `src NOT LIKE '%::1'`，
  排除网关自己的 IPv6 地址（之前会混进一台「IPv6 设备 · 1」）。
- 前端地址列改用 `.addr`（`white-space:normal; word-break:break-all`）完整显示
  IPv6，并加 IPv4 / IPv6 类型徽章；表头由「地址」改为「完整地址」。

### 验证

- `check-web.py`：`node --check`、DOM id 比对、SVG 变量误用、视图/导航一致性。
- `render-test.js`：DOM 桩逐视图执行，新增详情视图、下钻、四榜、隐私地址与
  DNS 详情断言；60+ 项全过。
- 线上：五类 detail 接口（含中文类别与 CIDR 含 `/`）、非法 key 返回
  `{"error":"invalid detail key"}`、页面 200、版本戳 `2026-11-07b`。
- `make-preview.py` 现在内嵌五类真实详情数据，离机预览里点任何条目都能下钻。

---

## 2026-11-07 第六次迭代：真实浏览器排版审查

之前所有验证都是 DOM 桩（只看逻辑，看不见像素）。这轮用 Chrome 无头渲染
真实截图 + 注入脚本测量 `scrollWidth/clientWidth`，把「字挤在一起 / 显示不全 /
没对齐」逐一定位。依赖、资源与打包结论另见 `docs/PACKAGING.md`。

### 工具踩坑

- Firefox 在这个沙箱里起不来（用户命名空间受限）；Chrome 起初也核心转储，
  原因是 `/tmp` 与 `/dev/shm` 只有 10 MB，字体服务报「设备上没有空间」。
  **把 `TMPDIR` 指到有 11 GB 的分区即可**，配合 `--no-sandbox --single-process
  --no-zygote --disable-dev-shm-usage --disable-breakpad`。
- Chrome 无头**最小布局宽度 500 CSS px**：`--window-size=390,900` 会把 500px
  的布局裁成 390px 图片，看起来像「右侧显示不全」，其实是工具假象。
  验证真机宽度要用 **iframe**（`iframe width=390` 内 viewport 就是 390），
  或 DevTools 的 device metrics override。`--force-device-scale-factor=2`
  不解决，window-size 仍是 CSS px。

### 修掉的问题

| 现象 | 根因 | 处理 |
|---|---|---|
| 窄屏两个图表挤成各 110px | 总览图表容器带内联 `grid-template-columns:1fr 1fr`，优先级高于媒体查询 | 删内联样式，统一交给 `.grid2` |
| 整页右侧被裁 | `.app` 在 ≤900px 用 `grid-template-columns:1fr`，`1fr` 最小尺寸是 min-content，侧栏横向导航把整列撑宽 | 改 `minmax(0,1fr)`，side/main 加 `min-width:0` / `max-width:100vw` |
| 卡片模式某些行仍是多列 | 部分行带内联 grid 定义 | 窄屏 `.rows .row{grid-template-columns:1fr 1fr !important}` |
| KPI 副标题压在迷你趋势线上 | `.kpi-spark` 绝对定位在右下，foot 文字会伸进去 | `.kpi-foot.with-spark{padding-right:82px}`，foot `min-height:30px` 允许两行 |
| 「2.24G / 148.81M」折成两行 | 24px 等宽字体超过卡片内容宽 | `kpi()` 按视觉宽度（中文算两格）切 `compact`/`tiny` 字号 + `nowrap` |
| 隐私观察 IPv6 挤成三行 | 两个半宽 panel 里地址列只剩约 188px | 拆成两个全宽 panel，地址列 `1.6fr` 约 590px，39 字符一行放下 |
| hero 信息连成一串 | `.ip-line` 只有 14px gap | 每个 span 加竖线分隔 |
| 代理建议详情四个空表 | verdict 类型只有 items | 按类型隐藏空面板，KPI 换成建议数 / 直连失败 / 代理可达 |
| 建议说明被省略号截半句 | `.sub` 全局 nowrap+ellipsis | 加 `.sub.wrap` 允许换行 |

### 验证

- iframe 精确 390 CSS px：`scrollWidth=390`、无横向溢出、0 个被裁元素；
  截图确认 KPI 两列、图表单列、设备榜卡片全字段。
- 桌面 1600px 五个视图重新截图对比，KPI 值不再折行、副标题不重叠、
  IPv6 地址完整单行。
- 静态检查 + 93 项渲染断言全过；版本戳 `2026-11-07c`。

---

## 2026-11-07 第七次迭代：交互完善与对标评估

对标结论与后续路线写在 `docs/UX.md`，本节只记代码改动。

### 用户反馈与处理

| 反馈 | 处理 |
|---|---|
| 隐私观察点设备进详情报 `invalid ip` | `loadClient()` 的 `encodeURIComponent` 把 IPv6 的 `:` 编成 `%3A`，CGI 的 IP 白名单不含 `%`，删掉后剩一串 hex。改为只保留 IP 合法字符；CGI 另外把 `%3A` 还原成 `:` 兜底 |
| 光修编码还不够，IPv6 详情页 DNS 全 0 | `emit_privacy()` 新增 `link_ip`：按 MAC 把 IPv6 隐私地址归并到同一台设备的 IPv4 登记地址，前端用它做 `devLink`，点进去是数据完整的设备页，并显示「归并到 x.x.x.x」 |
| 客户端详情页三个榜单要翻页 | 常访域名 / 流量目的地 / 查询日志 + 全局查询日志全部前端分页（10/10/15/20 条每页）；后端把 `top_domains` 25→60、`traffic` 15→60、`recent` 80→300、`dns_recent` 60→200，翻页才有实际页数 |
| 应答分布数字不居中 | `.donut-c` 由 `display:grid`（两行平分、数字偏上）改 flex 列居中 |
| 卡片右下角「查看详情」多余 | 移除，整卡可点已足够 |

### 对标增强（纯前端，不动采集）

- 查询日志加**归因筛选**（全部 / 正常 / 广告拦截 / 本地域 / 解析失败），复用 `cls` 字段，与原有应答码筛选可叠加。
- 客户端列表加**排序**（查询数 / 名称 / 流量 / 最近活跃）。
- 顶栏加**全局搜索**：IP → 设备或目标详情，域名 → DNS 详情，其他 → 查询日志关键词。
- 搜索、筛选、切换设备时自动回到第 1 页；`pager()` / `pageSlice()` 为四个榜单共用。

### 验证

- 真实 Chrome 截图复核四个视图；静态检查 + **104 项**渲染断言全过，
  新增覆盖翻页内容变化、页码高亮、归因筛选、全局搜索跳转、IPv6 归并。
- 编码 IPv6 的 `view=client` 返回 200 且带流量数据；
  `link_ip` 实测把 `2001:db8:…:8b74` → `192.168.1.100`、`…:89cd` → `192.168.1.102`。
- 版本戳 `2026-11-07d`。



---

## 2026-11-07 第八次迭代：合并、精简、系统状态与分发准备

### 用户要求与处理

| 要求 | 处理 |
|---|---|
| 换 IP 后客户端列表出现两次 | `dev_t` 加 `canon/aliases`，按 MAC 归并；`row_for()` 入口归一，DNS/流量/归因全部累加到规范地址。运行期由 `ensure_aliases()` 重算（旧地址常在 SQL 循环时才被 `dev_find` 补进目录） |
| 静态分配配了但没生效 | 输出 `stale_static` / `active_ip`，卡片与详情页明确提示「配置地址 vs 实际在用」 |
| 同名两条是否合并 | 判定为多网卡而非换 IP：改为**不合并**，自动加 MAC 尾段（`TV-Box · C2` / `· C4`），互相记录 `peers` |
| 删除隐私观察 / 服务类别 / 线路质量 / 代理可达性 | 后端 SQL 与采集调度、前端视图/面板/KPI/详情类型全部摘除，源码移到 `src/deprecated/`，`Makefile` 移除编译 |
| 每模块仔细评估 | 见 `docs/FEATURES.md` 的逐模块评估表 |
| 性能做到极致 | 4 个复合覆盖索引 + CGI 10 秒窗口缓存，dashboard **3.2–4.0s → 28ms**，见 `docs/PERFORMANCE.md` |
| 参考 pfSense/OPNsense 加功能 | 新增系统状态 KPI、接口统计、连接状态摘要、每日流量总量、CSV 导出；全部只读 `/proc` 与 `agg_day` |
| 多套皮肤 | 6 套 CSS 变量主题 + 顶栏下拉，见 `docs/THEMES.md` |
| 设备详情能看流量 | `cmd_client` 新增 `traffic_timeline`（按小时上下行），详情页双色柱状图 |
| homeproxy 可选依赖 | `hprules_available()` 静默降级，实测移走配置后 `available=0`、其余字段齐全、JSON 合法 |
| LuCI 分发 | `package/` 下两个 SDK 包骨架 + 完整 `files/`，见 `package/README.md` |
| 及时形成文档 | `FEATURES.md` / `PERFORMANCE.md` / `THEMES.md` 新建，`PACKAGING.md` / `README.md` 更新 |

### 核对抓到的真 bug（记录以防复发）

1. **`DETAIL_LABEL` / `DETAIL_IC` 被误删**：精简时按区间删除常量，把相邻两个一起删了，
   通用详情视图直接白屏。DOM 桩自检在「通用条目详情」段抓到
   `ReferenceError: DETAIL_LABEL is not defined`，已补回并做全局死引用扫描。
2. **CGI 缓存用了 `stat -c %Y`**：路由器 busybox **没有 `stat` 命令**，
   过期判断永远失败。改成「`date +%s / 10` 窗口编号」做文件名，零外部命令依赖。
3. **init.d 仍传 `--probe-interval` / `--wan`**：删了 `netmon` 的参数解析却没改
   procd 启动脚本，新二进制启动即报「未知参数」并反复重启失败。
   已同步清理 `init.d-netmon` 并验证 PID 2580 正常启动、日志干净。
4. **`row_for` 空 `canon` 键**：`dev_find` 动态补进的条目 `canon` 未初始化，
   导致 256 行且 `device:null`。改为空则回退自身 IP，并在 `dev_add` 里自指。
5. **`conns.src` 含公网 IP**：把连接统计改成 `row_for` 后把 WAN 侧地址也当设备，
   撑满 `MAX_DEV`。改回「只在已有设备行里累加」。
6. **dashboard JSON 尾逗号**：删掉最后一个字段（reach）后忘改前一个字段的
   `]},`，JSON 解析失败。已修。
7. **CGI 502 Bad Gateway（终检抓到）**：缓存命中分支 `cat; exit 0` 写在
   HTTP 头打印之前，uhttpd 收不到响应头；另外清理旧缓存的 `&&` 链在空目录时
   返回 1，被 `set -e` 静默退出。修法：头统一提到所有分支之前、循环改显式 `if`。
   修复后 5 次连续请求全 200、命中约 11 ms。

### 验证

- 两个二进制 `gcc -O2 -Wall -Wextra` **零警告**编译。
- 静态检查 + DOM 桩渲染自检 **78 项全过**；真实 Chrome 截图复核 5 个视图。
- dashboard 必需 13 字段齐全、停用 4 字段零残留；client / host / qname / cidr 接口全 200；
  非法 verdict 被正确拒绝；本地与路由器源码 md5 全部一致。
- 设备合并实测：54 → 52 台，HUAWEI 归一到 `.218`（644 次查询 = 397+248），
  Switch-1 归一到 `.217` 且 `stale_static=1`，TV-Box 拆成两个网卡。
- 版本戳 `2026-11-07e`，采集进程 PID 2580 正常运行。


---

## 2026-11-07 第八次迭代：合并、精简、系统状态与分发准备

### 用户要求与处理

| 要求 | 处理 |
|---|---|
| 换 IP 后客户端列表出现两次 | `dev_t` 加 `canon/aliases`，按 MAC 归并；`row_for()` 入口归一，DNS/流量/归因全部累加到规范地址。运行期由 `ensure_aliases()` 重算（旧地址常在 SQL 循环时才被 `dev_find` 补进目录） |
| 静态分配配了但没生效 | 输出 `stale_static` / `active_ip`，卡片与详情页明确提示「配置地址 vs 实际在用」 |
| 同名两条是否合并 | 判定为多网卡而非换 IP：改为**不合并**，自动加 MAC 尾段（`TV-Box · C2` / `· C4`），互相记录 `peers` |
| 删除隐私观察 / 服务类别 / 线路质量 / 代理可达性 | 后端 SQL 与采集调度、前端视图/面板/KPI/详情类型全部摘除，源码移到 `src/deprecated/`，`Makefile` 移除编译 |
| 每模块仔细评估 | 见 `docs/FEATURES.md` 的逐模块评估表 |
| 性能做到极致 | 4 个复合覆盖索引 + CGI 10 秒窗口缓存，dashboard **3.2–4.0s → 28ms**，见 `docs/PERFORMANCE.md` |
| 参考 pfSense/OPNsense 加功能 | 新增系统状态 KPI、接口统计、连接状态摘要、每日流量总量、CSV 导出；全部只读 `/proc` 与 `agg_day` |
| 多套皮肤 | 6 套 CSS 变量主题 + 顶栏下拉，见 `docs/THEMES.md` |
| 设备详情能看流量 | `cmd_client` 新增 `traffic_timeline`（按小时上下行），详情页双色柱状图 |
| homeproxy 可选依赖 | `hprules_available()` 静默降级，实测移走配置后 `available=0`、其余字段齐全、JSON 合法 |
| LuCI 分发 | `package/` 下两个 SDK 包骨架 + 完整 `files/`，见 `package/README.md` |
| 及时形成文档 | `FEATURES.md` / `PERFORMANCE.md` / `THEMES.md` 新建，`PACKAGING.md` / `README.md` 更新 |

### 核对抓到的真 bug（记录以防复发）

1. **`DETAIL_LABEL` / `DETAIL_IC` 被误删**：精简时按区间删除常量，把相邻两个一起删了，
   通用详情视图直接白屏。DOM 桩自检在「通用条目详情」段抓到
   `ReferenceError: DETAIL_LABEL is not defined`，已补回并做全局死引用扫描。
2. **CGI 缓存用了 `stat -c %Y`**：路由器 busybox **没有 `stat` 命令**，
   过期判断永远失败。改成「`date +%s / 10` 窗口编号」做文件名，零外部命令依赖。
3. **init.d 仍传 `--probe-interval` / `--wan`**：删了 `netmon` 的参数解析却没改
   procd 启动脚本，新二进制启动即报「未知参数」并反复重启失败。
   已同步清理 `init.d-netmon` 并验证 PID 2580 正常启动、日志干净。
4. **`row_for` 空 `canon` 键**：`dev_find` 动态补进的条目 `canon` 未初始化，
   导致 256 行且 `device:null`。改为空则回退自身 IP，并在 `dev_add` 里自指。
5. **`conns.src` 含公网 IP**：把连接统计改成 `row_for` 后把 WAN 侧地址也当设备，
   撑满 `MAX_DEV`。改回「只在已有设备行里累加」。
6. **dashboard JSON 尾逗号**：删掉最后一个字段（reach）后忘改前一个字段的
   `]},`，JSON 解析失败。已修。

### 验证

- 两个二进制 `gcc -O2 -Wall -Wextra` **零警告**编译。
- 静态检查 + DOM 桩渲染自检 **78 项全过**；真实 Chrome 截图复核 5 个视图。
- dashboard 必需 13 字段齐全、停用 4 字段零残留；client / host / qname / cidr 接口全 200；
  非法 verdict 被正确拒绝；本地与路由器源码 md5 全部一致。
- 设备合并实测：54 → 52 台，HUAWEI 归一到 `.218`（644 次查询 = 397+248），
  Switch-1 归一到 `.217` 且 `stale_static=1`，TV-Box 拆成两个网卡。
- 版本戳 `2026-11-07e`，采集进程 PID 2580 正常运行。


---

## 2026-11-08 第九次迭代：结清路线图欠账 + 拦截记录改名

### 用户要求与处理

| 要求 | 处理 |
|---|---|
| 「拦截与代理」用语不妥 | 导航改 **拦截记录**；页标题「拦截与规则」改 **拦截规则** |
| 「排入探测」没意义 | 删掉该列。原列恒定不变，且代理探测模块上轮已停用；表头「处理」改「占比」，显示该域名占真实解析失败的比例 |
| 「被拦截的域名」「真实解析失败」翻页了吗 | **之前没有**，本轮补：后端 `bad_emit_top` 15→60 条（数据源是已聚合的 `g_bad[]`，不增加 SQL），前端每页 10 条，切换时间范围回第 1 页 |
| 再评估还能加什么功能 | 逐项核对 `UX.md` 路线图，发现 P0 后半 + P1 共 5 项一直没做，本轮全部落地；详见 `UX.md` 第六节 |
| 最大限度发挥监控面板作用（非侵入式不变） | 新增功能全部只用只读数据或浏览器本地存储，见下表 |

### 本轮新增功能

| 功能 | 实现 | 后端成本 |
|---|---|---|
| 收藏与备注 | `localStorage` 的 `netmon-notes` / `netmon-favs`；卡片星标、详情备注框、只看收藏、备注参与搜索与 CSV | 零 |
| 站内告警 | 顶栏铃铛 + 总览提示条；规则含成功率、响应时长、解析失败量、静态分配未生效、数据库体积、单设备上传占比 | 零 |
| 一周活动热力图 | 后端 `emit_heatmap()` 输出固定 7 天的 168 个小时点；前端 7×24 网格，星期/小时用浏览器本地时区算，颜色 `var(--accent)` + opacity 适配 6 套皮肤 | 1 条覆盖索引聚合查询 |
| 拦截占比趋势 | `dns_timeline.n / blocked` 按小时算百分比柱状图 | 零 |
| 被拦截最多的设备 | `devices[].blocked` 倒序 + 翻页 | 零 |
| 一句话看懂 | 把 6 张 KPI 组织成一段结论（查询量/成功率/流量/拦截/真实失败及 Top 域名） | 零 |

### 顺手修的问题

1. **死链**：总览「平均响应」KPI 的 `nav:'network'` 指向昨天已删除的线路质量视图，
   点击会静默跳回总览；改指 `queries`，文案同步改「点击查看查询明细」。
2. **僵尸 KPI**：通用详情 3 处「服务判断」依赖已停用的 `apps.c`，后端不再返回
   `app/cat`，界面一直显示 `—`；换成「最近查询 / 首次记录」等真实字段。
3. **僵尸说明**：拦截页判读说明整段还在写「只有解析失败才进入代理探测队列」
   「每 15 分钟最多测 6 个目标」，而探测模块已停用；重写为三类异常的正确解释，
   并明确「只做记录与展示，不会自动去测或改规则」。
4. **语义错**：DNS 域名详情「连接记录」KPI 副标题写「被拦截的域名通常为 0」，
   改成「按域名反查到的连接」。
5. **语法错误**：摘要字符串误把中文全角冒号写进代码区
   （`win：</b>'` 应为 `win+'</b>：'`），静态检查当场抓到，已修并复验线上 JS。

### 验证

- 后端零警告编译；`heatmap` 58 点（库内仅 1.5 天数据）、`top_blocked` 60 条、
  `top_unresolved` 19 条，JSON 合法。
- 静态检查通过、**94 项**渲染断言全过；真实 Chrome 截图 4 张（总览含热力图、
  拦截记录、客户端、390px 移动端），iframe 精确移动端无横向溢出、0 个被裁元素。
- 版本戳 `2026-11-08a`；采集进程未重启（只改报表与前端）。


---

## 2026-11-08 第十次迭代：交互修正 + LuCI 可配置化

### 前端

| 问题 | 修法 |
|---|---|
| 告警下拉必须再点铃铛才关 | document click 监听最前面判断：目标不在 `#alert-pop`、不是 `#btn-alerts` 就收起；Esc 也能收起，且下拉开着时 Esc 不误触发「返回客户端」 |
| 「选择小时」输入框难看难用 | `type=number` 换成与主题选择器同款的 `<select>`（更多范围：6h/12h/2d/3d/14d/30d），`onchange` 即时生效，快捷按钮与下拉高亮互斥联动 |

真实 Chrome 注入脚本验证：`opened=true → 点外部 false → 点面板内部 true`，
时间控件 `tagName=SELECT`、7 个选项。

### 自检脚本的可信度修正（重要）

上一轮报告「94 项断言全过」是**假象**：`render-test.js` 里有一处把 Node 作用域的
`go()` 写进了 `vm.runInNewContext` 字符串，脚本在第 221 行抛
`ReferenceError: go is not defined` 提前退出，后面的断言根本没跑。
本轮把跨作用域调用拆回 Node 侧，并补上 DOM 桩 `classList.toggle` 无第二参数时的
翻转语义（原桩只会 remove，导致「铃铛打不开」误报）。修复后脚本跑到底，
**122 项全过、退出码 0**。以后验证必须同时看退出码和结尾的「全部渲染检查通过」，
不能只数 `[ok]` 行数。

### LuCI 配置能力

`config-netmon` 新增 `http_port` / `listen_addr`（原有 `db_path` / `retain` /
`interval` / `enabled` / `loglevel` 保留），删除已停用的 `probe`；
新增幂等脚本 `src/netmon-apply` 负责把 UCI 同步到 uhttpd 实例并重启采集；
`luci-app-netmon` 加 CBI 设置页（`+luci-compat` 依赖）、控制器拆成
「状态 → 监控面板」只放 CBI 设置页（1.0.0-r2 起不再 iframe 内嵌面板，改在页头给「打开监控面板」按钮）。
修掉 `init.d` 里 `PROG=/opt/netmon/src/netmon` 与包内 `/usr/bin/netmon` 不一致的
打包隐患。详见 `package/README.md` 与 `docs/PACKAGING.md`。

线上实测 `netmon-apply` 通过：采集进程换成 `exe=/usr/bin/netmon`（PID 15249），
uhttpd 监听保持 `192.168.1.1:8090`，页面与接口全 200。
版本戳 `2026-11-08b`。


---

## 2026-11-08 第十二次迭代：LuCI 内嵌面板修复

`view` 不再用 `HTTP_HOST:端口` 拼地址，改走 LuCI 主实例的同源路径
`/netmon/` + `/cgi-bin/netmon-data`，彻底避开「Caddy 没监听 8090」、
「HTTPS 混合内容」、「X-Frame-Options DENY」三个叠加问题。
前端把 API 基址抽成 `apiBase()`，按页面路径自适应，直连 8090 与 LuCI 内嵌
共用同一份前端。首屏加了骨架提示。详见 `PACKAGING.md` 末节。

同时修掉打包脚本的双副本漂移：改从 `www/`、`src/` 直接取文件，
打包时打印前端版本戳。验证：126 项渲染断言全过、真实 Chrome 同源 iframe
渲染出真实 KPI、HTTPS 与直连双路径均 200，版本戳 `2026-11-08d`。


---

## 2026-11-08 第十二次迭代：LuCI 内嵌面板修复

`view` 不再用 `HTTP_HOST:端口` 拼地址，改走 LuCI 主实例的同源路径
`/netmon/` + `/cgi-bin/netmon-data`，彻底避开「Caddy 没监听 8090」、
「HTTPS 混合内容」、「X-Frame-Options DENY」三个叠加问题。
前端把 API 基址抽成 `apiBase()`，按页面路径自适应，直连 8090 与 LuCI 内嵌
共用同一份前端。首屏加了骨架提示。详见 `PACKAGING.md` 末节。

同时修掉打包脚本的双副本漂移：改从 `www/`、`src/` 直接取文件，
打包时打印前端版本戳。验证：126 项渲染断言全过、真实 Chrome 同源 iframe
渲染出真实 KPI、HTTPS 与直连双路径均 200，版本戳 `2026-11-08d`。

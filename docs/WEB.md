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
  │ GET /cgi-bin/data?since=86400&fam=6
  └──────────────► /www/netmon/cgi-bin/data  (sh 脚本)
                     └─ exec netmon-report dashboard --since 24h --fam 6
                          └─ 只读打开 SQLite，输出完整 JSON
```

**一个视图一个请求。** 路由器上每次 CGI 都要 fork 进程，
拆成多个接口会明显变慢。所以总览类视图由 `dashboard` 一次性输出
概况 + 流量排行 + 趋势 + DNS + 设备；设备详情、条目详情各自用
`client` / `detail` 子命令。CGI 有 10 秒窗口文件缓存，文件名包含
`since` 与 `fam`，不同协议族视图不会串数据。

`fam` 只接受字面量 `4` / `6` / `0`（空等同 all），其余值直接丢弃，
不会拼进 shell 命令。

---

## 为什么不用现成方案

| 方案 | 问题 |
|---|---|
| Grafana / Prometheus | 要额外装 100MB+ 组件，路由器上不合适 |
| LuCI 应用 | 侵入 LuCI，升级固件会被覆盖，且要写 Lua |
| 前端框架（Vue/React） | 要 CDN 或打包，路由器离线场景不可用 |
| **原生 JS 单文件** | ✅ 约 162KB，无依赖，uhttpd 直接托管 |

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

### 视图结构

左侧导航 8 项 + 2 个下钻视图，全部走 hash 路由：

| hash | 视图 | 主要数据 |
|---|---|---|
| `#/overview` | 总览 | `summary` / `trend` / `devices` / `dns` / `dns_timeline` / `block_stats` / `heatmap` |
| `#/clients` | 客户端 | `devices` + 设备目录 |
| `#/client/<ip>` | 设备详情 | `client` 接口 |
| `#/blocks` | 拦截记录 | `block_stats` / `hp` |
| `#/traffic` | 流量排行 | `top_cidr` / `top_host` / `traffic_hosts` / `devices` |
| `#/dns` | DNS 解析 | `dns_servers` / `stack` / `dns_recent` |
| `#/queries` | 查询日志 | `dns_recent` |
| `#/domains` | 域名排行 | `dns` / `top_host` |
| `#/system` | 系统信息 | `sysinfo` / `daily` / `summary` |
| `#/detail/<type>/<key>` | 条目详情 | `detail` 接口（`host` / `qname` / `cidr`） |

### 顶部指标卡

| 卡片 | 数据来源 |
|---|---|
| 查询量 | `summary.queries` |
| 活跃客户端 | `summary.clients` |
| 拦截数 | `summary.blocked`（口径为 `nx + ref`） |
| 平均响应 | `summary.avg_ms`，副标题带 `summary.reply_rate` |
| 总流量 | `summary.bytes_out + bytes_in`，副标题带 IPv6 占比 |
| 数据库 | `summary.db_bytes` |

### 流量趋势

Canvas 手绘，**不用图表库**（省 200KB 依赖）。

- 绿柱 = 下行，蓝柱 = 上行，堆叠显示
- Y 轴自动缩放，标注 4 档
- X 轴最多标 6 个时间点
- 数据来自 `trend`，每行带 `fam`；v4 / v6 合并画总量，协议族标识在榜单行上

### 流量排行

- **按网段**：`top_cidr`，带横向进度条与 `IPv4` / `IPv6` 徽标；
  v4 按 `/24`、v6 按 `/48` 聚合
- **按域名**：`top_host` / `traffic_hosts`，靠 DNS 嗅探反查；
  A/AAAA 应答都进反查表，IPv6 目的地址也能显示域名
- **按设备**：按 LAN 侧地址归属，上下行分开，入向连接的方向已归一

### DNS 解析

回答「查询到底发给了谁」的承接页：

1. **4 张 KPI**：DNS 服务器数、DNS 查询量、平均响应、IPv6 查询占比。
2. **服务器榜**：名称 + IP（带协议族徽标与端口）、查询量、占全网查询的比例、
   平均耗时。点任意一行跳到查询日志，并自动带该服务器筛选。没有服务器数据时
   给一句引导（说明该列 1.1.0 起才有），不显示空白表格。
3. **协议栈对比**：v4 / v6 的连接数、上行、下行。
4. **固定说明**：写清服务器地址取自 DNS 报文本身（观测事实，不是推断），
   以及为什么这一版不提供缓存命中率。

> 页面**不显示缓存命中率**。1.1.0 早期版本实现了「上游转发观测 + 时延 + TTL
> 衰减」的推断，实测在这类环境（DNS 走代理 / DoH）判定覆盖率不足 2%，
> 数字会误导人，已整体移除：`cache_state` / `cache_ev` 列、`cache` JSON、
> 环形图、判定依据面板、查询日志的缓存列与缓存筛选都已删掉。

服务器名称是纯展示映射：本机接口地址 → 端口 53 显示「本机 dnsmasq」，
其它端口显示「本机解析服务」（同一台机器上代理自己的 DNS 口不该被叫成
dnsmasq）；内置公共解析器表（阿里 / 腾讯 / 114DNS / Cloudflare / Google /
Quad9 / 百度，含 IPv6 地址）→ 对应名称；私有 / ULA / link-local / 回环 →
「局域网 DNS」；其余原样显示 IP。**这张表不参与任何采集或转发决策。**

### 查询日志

列：`时间 | 客户端 | 域名 | 类型 | 状态 | DNS 服务器 | 耗时 | 应答 IP`。

- **DNS 服务器**列显示 `server_label`（识别名称），悬停可看原始 IP；
  `server_fam=6` 带 `IPv6` 徽标。
- **应答 IP** 显示 `answers`（新行是真实 IP，逗号分隔；老行归一成「—」），
  有 TTL 时下面一行小字 `TTL <n>s`。
- 顶部只有服务器下拉（按 `server` 筛选），与应答码、归因筛选共同生效。
- **「不含重试 / 含重试」** 控制「域重试」行是否出现。被 reject 的域名解析失败后，
  客户端会把搜索域补到后面再问一次（`dig.bdurl.net` → `dig.bdurl.net.lan`），
  这类行和原查询是同一件事，默认折叠；要对照时切「含重试」。
  真正的内网主机名（`nas.lan`）不属于重试，任何时候都显示。
- 筛选条件（`rcode` / `cls` / `server` / `retry` / 页码）写进 URL hash，
  刷新浏览器不丢。
- 「导出」只导出**当前筛选后的行**，列包含服务器、TTL、耗时、应答 IP，
  和页面看到的一致。

### 域名排行 / 拦截记录

- **域名排行**：按查询量 / 按流量，数据来自 `dns` 与 `top_host`。
- **拦截记录**：把 DNS 失败拆成广告/规则拦截、本地域名、真实解析失败三类。
  归因在**采集写库时**就算好并存入 `dnsq.hp`，不是查询时按当前规则反推 ——
  规则集改了，历史行的结论不会跟着变。老库（无该列）由报表按 rcode + 后缀
  兜底，口径与 `hprules_classify()` 一致。

### 设备详情 / 条目详情

设备详情（`client` 接口）：

- 概况含 `v4_queries` / `v6_queries` / `v4_bytes_*` / `v6_bytes_*`；
- 查询活动、流量趋势、Top 域名、查询类型、应答码、最近查询、流量目的地；
- 最近查询里也带服务器与应答 TTL；
- 流量目的地面板标题是「IPv4 + IPv6 · 按 LAN 侧地址归属」。

条目详情（`detail` 接口，`host` / `qname` / `cidr`）：

- 连接的 `fam` 与 LAN 侧 `client_ip`；
- DNS 明细的 `server` / `server_label` / `ttl`；
- 关联设备、分时段流量。

两个详情页的返回按钮都回到来源视图（查询日志 / 域名排行 / 设备列表），
不再固定跳客户端列表。

### 系统信息

接口收发、conntrack 总量与 TCP/UDP/ESTABLISHED、`ct_v4` / `ct_v6`、
每日流量总量、数据库体积与行数、DNS 服务器数、IPv6 连接数。
全部只读 `/proc`。

### 时间范围与自动刷新

右上角 1 小时 / 24 小时 / 7 天，切换后重新请求。

每 10 秒刷新一次，但**只刷当前视图需要的数据**：设备详情只刷 `client`、
条目详情只刷 `detail`、其余只刷 `dashboard`。输入框正在输入时跳过本轮，
不打断输入与翻页。左上角圆点：绿色=正常，红色=请求失败。

失败时页面顶部显示红色提示条，明确告诉你问题在哪
（接口 500 / JSON 解析失败 / 请求超时），不会白屏。

### 告警与空状态

- 站内告警每条都是可点元素，点击后跳到对应视图并自动应用筛选
  （例如「DNS 成功率偏低」→ 查询日志 + 失败归因）。
- 空库时总览顶部显示三步引导：确认服务在跑 → 确认流量经过本机 → 看系统信息。
- 全局搜索：DNS 服务器 IP → DNS 解析页并筛选该服务器；已知设备 IP → 设备详情；
  IPv6 客户端地址 → 查询日志关键词；未知 IP → 条目详情。

### 移动端与皮肤

390px 下切顶部导航与单列卡片，表格行由 `labelizeRows()` 自动带列名标签；
服务器 / TTL / 应答 IP 列在窄屏不隐藏。6 套皮肤全部走 CSS 变量，
新增颜色只引用已有变量，对比度不变。

---

## 接口字段

### CGI 参数

| 参数 | 取值 | 说明 |
|---|---|---|
| `since` | 60 ~ 2592000（秒） | 时间窗口，非法值回落到 24h |
| `view` | `client` / `detail` / 空 | 空 = dashboard |
| `ip` | IPv4 / IPv6 文本 | `view=client` 必填 |
| `type` | `host` / `qname` / `cidr` | `view=detail` 必填 |
| `key` | 条目键 | `view=detail` 必填 |
| `fam` | `4` / `6` / `0` / 空 | 只透传白名单值；`0` 与空等同 all |

`fam` 会进入 dashboard 的缓存文件名。注意：`dashboard` 本身**不按 fam 缩数据**，
始终返回全量 + `stack` 分栈；真正按协议族过滤的是命令行
`dns / devices / top / trend`。内置页面也是全量展示、逐行标协议族。

### dashboard

顶层：

| 字段 | 说明 |
|---|---|
| `now_ms` / `since_ms` | 本次计算的服务端时间与窗口起点 |
| `summary` | 全库总量 + 窗口内概况，见下表 |
| `stack` | v4 / v6 分栈：`{v4:{conns,queries,bytes_out,bytes_in,clients}, v6:{…}}` |
| `dns_servers` | 服务器聚合，最多 20 条 |
| `top_cidr` | 目的网段榜，每行 `k, conns, out, in, fam` |
| `top_host` | 域名榜，每行 `k, conns, bytes, fam` |
| `trend` | 小时趋势，每行 `t, conns, out, in, fam` |
| `dns` | 域名解析质量榜，每行 `q, n, replied, noerror, nx, ref, ms, ms_max, cls, rule` |
| `devices` | 设备全量，见下表 |
| `dns_timeline` | DNS 小时曲线，每行 `t, n, blocked, ms` |
| `dns_recent` | 最近 200 条查询，见下表 |
| `sysinfo` | 系统状态，见下表 |
| `daily` | 每日流量总量，每行 `t, out, in, conns` |
| `traffic_hosts` | 目标流量榜，每行 `k, conns, out, in, bytes, fam` |
| `block_stats` | 拦截归因：`total, blocked, local, unresolved, blocked_pct, top_blocked[], top_unresolved[], by_rule[]` |
| `heatmap` | 7 天活动热力图，每行 `h, n` |
| `hp` | homeproxy 规则版本与变更事件 |

`summary`：

| 字段 | 含义 |
|---|---|
| `conns` / `dns` / `probes` | 全库连接 / DNS / 探测总行数（不受窗口限制） |
| `db_bytes` | `page_count × page_size` |
| `queries` / `clients` | 窗口内 DNS 查询数 / 活跃客户端数 |
| `catalog` | 设备目录规模 |
| `replied` / `nx` / `ref` | 收到应答 / NXDOMAIN / REFUSED |
| `blocked` | `nx + ref`（页面口径） |
| `reply_rate` / `avg_ms` | 应答率 / 平均耗时 |
| `win_conns` | 窗口内连接数 |
| `bytes_out` / `bytes_in` | 窗口内上下行 |
| `v6_bytes_out` / `v6_bytes_in` / `v6_conns` / `v6_queries` | IPv6 分栈量 |
| `dns_servers_n` | 窗口内出现的 DNS 服务器数 |

`dns_servers[]`：

| 字段 | 含义 |
|---|---|
| `ip` / `label` / `fam` / `port` | 服务器地址、展示名、协议族、端口 |
| `n` | 查询数 |
| `avg_ms` | 该服务器的平均应答耗时 |

> 1.1.0 早期版本这里还有 `hit` / `miss` / `local` / `unknown` / `hit_rate` /
> `upstreams`，随缓存推断一起移除。

`dns_recent[]`（每行在旧字段之外新增）：

| 字段 | 含义 |
|---|---|
| `server` / `server_label` / `server_fam` | 查询发往的 DNS 服务器、展示名、协议族 |
| `ttl` | 首个 A/AAAA 应答 TTL；`null` = 未知 |
| `answer_cnt` | 本条应答的 A/AAAA 条数 |
| `client_fam` | 客户端协议族 |
| `answers` | 真实应答 IP（逗号分隔）；旧行归一为空串 |
| `cls` / `rule` | 归因分类与命中规则 |
| `retry` | `1` = 本地域伴生重试（`mail.example.com.lan`），`0` = 正常查询或真实内网名 |

`devices[]`（每行在旧字段之外新增）：

| 字段 | 含义 |
|---|---|
| `fam` | 该设备最近活动地址的协议族（4 / 6） |
| `v6_conns` / `v6_bytes_out` / `v6_bytes_in` | 该设备的 IPv6 连接数与上下行 |
| `device` | 设备对象：`ip, name, display, mac, vendor, kind, source, aliases, peers, multi_nic` |

`sysinfo`：

| 字段 | 含义 |
|---|---|
| `uptime_s` / `load` | 运行时长与 1/5/15 分钟负载 |
| `mem_total_kb` / `mem_avail_kb` | 内存 |
| `disk_total_mb` / `disk_free_mb` | 数据盘容量 |
| `ct_total` / `ct_tcp` / `ct_udp` / `ct_established` | conntrack 总量与分类 |
| `ct_v4` / `ct_v6` | conntrack 行按 `ipv4` / `ipv6` 开头拆分 |
| `interfaces[]` | `name, rx_bytes, rx_pkts, rx_err, rx_drop, tx_bytes, tx_pkts, tx_err, tx_drop` |

### client（设备详情）

顶层：`device`、`since_ms`、`summary`、`timeline`、`traffic_timeline`、
`top_domains`、`qtypes`、`rcodes`、`recent`、`block_stats`、`traffic`。

`summary` 在旧字段之外新增：

| 字段 | 含义 |
|---|---|
| `v4_queries` / `v6_queries` | 该设备 v4 / v6 DNS 查询数 |
| `v4_bytes_out` / `v6_bytes_out` | 该设备 v4 / v6 上行 |
| `v4_bytes_in` / `v6_bytes_in` | 该设备 v4 / v6 下行 |

`traffic[]` 每行新增 `fam`；`recent[]` 的 `answers` 语义同 `dns_recent`，
并带 `server` / `server_label` / `server_fam` / `ttl` / `answer_cnt`。

### detail（条目详情）

顶层：`type`、`key`、`title`、`since_ms`、`summary`、`devices`、`timeline`、
`conns`、`dns`。

| 位置 | 新增字段 |
|---|---|
| `conns[]` | `fam`、`client_ip`（LAN 侧地址） |
| `dns[]` | `server`、`server_label`、`ttl`；`answers` 归一规则同上 |

### 字段约定

- SQL NULL 一律输出 JSON `null`，不写 0 —— 「没测到」和「测得 0」必须能区分。
- 新字段只增不改；旧字段语义保持不变。`answers` / `upstream` 的旧值在报表读时归一。
- 所有字符串都过 `json_esc()`；所有外部输入走 `sqlite3_bind` 或白名单。

## 修改前端

### 改完怎么部署

```bash
# 本地改 www/index.html 或 www/cgi-bin-data
cd netmon/www
tar cf - index.html cgi-bin-data \
  | ssh -F ~/.ssh/netmon.conf router 'cd /www/netmon && tar xf - && \
      mv cgi-bin-data cgi-bin/data && chmod 755 cgi-bin/data'
```

**不用重启任何服务**，刷新浏览器即可看到。

### 改接口返回的字段

改 `src/netmon-report.c` 的 `cmd_dashboard()`，然后：

```bash
cd src
tar cf - netmon-report.c | ssh -F ~/.ssh/netmon.conf router 'cd /opt/netmon/src && \
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
# 带协议族参数（dashboard 仍返回全量，fam 只影响缓存 key）
curl "http://192.168.1.1:8090/cgi-bin/data?since=86400&fam=6" | head -c 300
```

- 返回 JSON → 前端问题，看浏览器 Console
- 返回 `{"error":"query failed"}` → 数据库或 netmon-report 问题
- 返回 `{"error":"netmon-report not found"}` → `/usr/bin/netmon-report` 丢了

### 数字看起来不对

```bash
netmon-report dashboard --since 24h | head -c 500
netmon-report servers --since 24h        # 服务器维度对不上时先看这个
```

和页面对比。如果命令行对但页面不对，是前端渲染问题。

`servers` 的数字比页面少是正常的：页面统计的是全窗口，命令行默认 24 小时；
另外只有 1.1.0 之后写入的行带 `server_ip`，更早的历史行不参与服务器聚合。

### CGI 报 500

```bash
# 手动跑一遍，看 stderr
QUERY_STRING="since=86400" sh -x /www/netmon/cgi-bin/data
```

---

## 后续可加的功能

按价值排序（已实现的告警跳转、CSV 导出、皮肤切换、DNS 服务器列、
按视图刷新、URL 状态保持都不再列）：

1. **实时连接列表** —— 当前活跃连接（需要从 conntrack 现读，不是查库）。
2. **历史对比** —— 今天 vs 昨天同时段，或本次窗口 vs 上一个等长窗口。
3. **协议栈切换** —— `fam` 参数与 CLI 过滤已经具备，前端还缺一个
   「v4 / v6 / 全部」开关；现在只能看全量 + 逐行徽标。
4. **DNS 服务器名称自定义覆盖** —— 目前公共解析器名称是内置表，
   想给它起别名只能改代码；可以做成 UCI 的 `dns_labels` 列表。
5. **查询日志导出更长的历史** —— 页面只拿最近 200 条，导出也受这个上限限制；
   需要更长的 CSV 得走 `netmon-report dns --since 7d` 之类的命令行。
6. **VLAN / IPv6 扩展头嗅探** —— 现在 BPF 固定按基础头取端口，
   带 802.1Q 或扩展头的 DNS 帧收不到；扩写要谨慎，避免误判。

都不难，但每次改动都要重新验证「不许断网」的底线：监控只能旁路只读，
不能因为新增功能去改 dnsmasq、防火墙或转发路径。

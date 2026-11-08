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

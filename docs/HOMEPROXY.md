# netmon × homeproxy：规则同步、DNS 归因与代理可达性建议

## 这套机制回答什么问题

1. 面板上大量「域名解析失败」，到底是网络坏了，还是 homeproxy 主动拦截的结果？
2. 排除拦截之后仍然失败的域名，走代理能不能通？哪些该补进 Proxy 规则？
3. homeproxy 的过滤规则改了，面板能不能马上跟着变，而不是拿一份过期名单糊弄人？

设计底线：**纯旁路**。不写 homeproxy 配置、不改防火墙、不改路由规则、不参与 DNS 转发；
主动探测也只发极少量 HTTP 请求，且结论只做建议，**绝不自动改代理规则**。

---

## 一、规则从哪里来，怎么保持同步

| 来源 | 内容 | 读取方式 |
|---|---|---|
| `/etc/config/homeproxy` | `action='reject'` 的 `dns_rule` / `routing_rule`（label、显式域名、引用的 rule_set） | C 直接解析 UCI 文本 |
| `/var/run/homeproxy/cache.db` | sing-box 缓存的各 rule_set 二进制（`.srs`） | 按明文键 `cfg-<name>-rule` 定位 SRS 段 → `sing-box rule-set decompile` 反编译成 source JSON |

**不使用任何固化的规则副本**。实测 `category-ads-all` 的 SRS 解压后是压缩 domain trie + 位图，
不是明文；自己解既容易错，也会随 sing-box 版本失效。官方 `decompile` 输出的 source JSON
里域名/正则是明文，扫描 `"..."` 字符串按特征分类即可，无需完整 JSON 解析器。

### 规则指纹

```
指纹 = FNV1a(/etc/config/homeproxy 全文)
     + FNV1a(所有 reject 段引用的 rule_set 在 cache.db 里的 SRS 段内容)
```

要点：

- **只看内容不看 mtime**。sing-box 会定期写 cache.db，用 mtime 会造成无意义重编译。
- **规则集名单必须从 UCI 原文重新解析**，不能用内存里的 `g_meta`。
  否则用户新增一条引用新 rule_set 的 reject 规则时，本轮指纹仍按旧清单算，
  新规则集要晚整整一个周期才被发现（审查时修掉的真 bug）。
- 任一分量变化 → 重新反编译变化的 SRS → 重建匹配表 → 写一条 `hp_event` 变更记录。
- 采集进程每 **30 秒**检查一次；`netmon-report` 每次 CGI 请求也检查一次。
  所以规则改动最迟 30 秒生效，且下一次刷新页面一定用的是新规则。
- 反编译临时文件带 PID，避免采集进程和 CGI 进程同时 decompile 时互相删文件。

### 变更记录

`hp_event(ts_ms, fp, detail, rules, patterns)`，前端「拦截与代理」页展示最近 12 条。
`detail` 会写明变化类型与前后数量，例如：

```
配置变更：规则 3→3，模式 907→907
规则集更新：规则 3→4，模式 907→1312
```

**实测验证**：往 `/etc/config/homeproxy` 追加一条无害注释（UCI 忽略注释），
35 秒后自动记录一条「配置变更」；恢复文件后再记录一条；md5 校验一致，
sing-box 进程未受影响。

---

## 二、DNS 异常归因

`hprules_classify(qname, rcode)` 返回四类：

| 值 | 含义 | 判定依据 | 是否需要关注 |
|---|---|---|---|
| `HP_OK` | 正常 | rcode=0 且未命中规则 | 否 |
| `HP_BLOCKED` | 广告 / 规则拦截 | 命中 AdsAll 后缀/正则、显式 reject 规则；`rcode=5` 兜底 | **否，策略生效** |
| `HP_LOCAL` | 本地搜索域 | `.lan` / `.local` / `.arpa` 等后缀 | 否，客户端先查「域名.lan」的伴生 NXDOMAIN |
| `HP_UNRESOLVED` | 真实解析失败 | 排除以上后仍 rcode≠0 | **是，进入代理探测队列** |

实测（24h 窗口）：约 **51% 的 DNS 异常是广告拦截**，`rcode=5(REFUSED)` 的
53,582 条里有 53,578 条（100%）命中 AdsAll 规则；NXDOMAIN 则几乎全是 `.lan`
伴生查询和 IoT 设备上线探测域名。把这两类从「网络故障」里摘出去，
面板才不会天天误报。

前端「拦截与代理」页展示：拦截/本地域/真实失败三类的次数与占比、
被拦截域名 Top 15（含命中的规则名）、真实失败 Top 15、按规则聚合。
查询日志、域名排行、设备详情里每条异常都带归因徽章。

---

## 三、代理可达性探测

### 候选怎么选

- `dnsq` 中 24h 内 `rcode IN (2,3,5)` 按域名聚合，只留 `HP_UNRESOLVED`；
- `conns` 中 `byt_r = 0`（发出去一个字节都没回来）的目的地，排除内网/本机地址；
- 合并、按观测次数排序、**同一目标 24 小时冷却**、每轮最多 6 个。

### 双路径探测

| 路径 | curl 参数 | 目的 |
|---|---|---|
| 直连 | `--interface <WAN_IP>` + `-k` | 绕过 homeproxy TUN，测真实物理线路 |
| 代理 | `-x http://127.0.0.1:5330` + `-k`，**不加** `--interface` | 测 homeproxy mixed 端口 |

端口取自 `homeproxy.infra.mixed_port`，可用 `--proxy` 覆盖。
两条路径都请求 `https://<host>/`（IP 字面量用 http），`--range 0-0` 只取首字节，
单目标超时 6s / 10s。

**可达判定只看 `200 <= code < 500`**。5xx（尤其 502/503/504）是 sing-box 返回的
网关错误页，说明代理自己也连不上目标；早期用 `code > 0` 判定，把 502 当成
「代理可达」，凭空生成了错误的加规则建议，审查时已收紧。

### 结论与建议

| verdict | 含义 | 建议 |
|---|---|---|
| `need_proxy` | 直连不通、代理可达 | **建议加入 Proxy 规则**（前端高亮） |
| `both_ok` | 两条路都通 | 暂不需要加规则 |
| `direct_only` | 仅直连通 | 走代理反而失败，别加 |
| `both_fail` | 两条路都不通 | 站点下线 / 非 HTTP 服务，加规则没用 |

结果写 `reach(ts_ms, host, reason, attempts, direct_*, proxy_*, verdict, note)`，
前端按 host 取最新一次，`need_proxy` 排最前。

### 开销与开关

```
netmon --proxy http://127.0.0.1:5330 \
       --reach-interval 900 \      # 15 分钟一轮
       --reach-limit 6             # 每轮最多 6 个；0 = 关闭探测
```

默认即上述值。所有探测都在 `run_probe()` 之外独立调度，不占采样热路径；
curl 用参数数组/`strpbrk` 白名单调用，不经过 shell 注入面。

---

## 四、代码结构

| 文件 | 职责 |
|---|---|
| `src/hprules.c/.h` | 规则同步、指纹、归因匹配（采集与报表共用同一份实现） |
| `src/reach.c/.h` | 候选筛选、双路径探测、结论落库 |
| `src/probe.c` | 新增 `probe_http_proxy()`，与既有 `probe_http()` 共用 curl 构造 |
| `src/db.c` | 新增 `reach` / `hp_event` 表、`db_add_reach()` / `db_add_hpevent()` / `db_foreach()` |
| `src/netmon.c` | 30 秒规则指纹轮询、15 分钟探测调度、变更事件落库 |
| `src/netmon-report.c` | `collect_bad()` 归因聚合；dashboard 输出 `block_stats` / `hp` / `reach` |
| `www/index.html` | 「拦截与代理」视图 + 全局归因徽章 |

### 命名约定

- 后端 `HP_*` 为归因枚举，`verdict` 为探测结论，`cls` 为前端归因字段；
- 数据库 `reach` 表只存观测结果，`hp_event` 只存规则变更，互不混用；
- 规则模块的对外函数一律 `hprules_` 前缀，探测模块一律 `reach_` 前缀；
- 界面文案不用内部术语：说「按小时统计 · 共 25 小时」，不说「25 个小时桶」；
  说「规则版本」，不说「指纹」；说「规则条目」，不说「模式」。

---

## 五、已知限制

- 设备详情里的流量只统计该设备 **IPv4** 地址发起的连接；IPv6 源地址不归属到 IPv4 设备。
- `decompile` 需要 `sing-box` 在 PATH 且 `cache.db` 可读；若规则集缓存缺失，
  保留上一次成功加载的匹配表，并用 `rcode=5` 兜底拦截判定，不会整页报错。
- Go 正则与 POSIX ERE 不完全兼容（如 `(?:...)`），`regcomp` 失败的模式会跳过；
  这类规则仍可通过同规则集里的域名后缀命中。
- 代理探测是 HTTP(S) 层判定，非 HTTP 协议（游戏、PT、自定义 TCP）会得 `both_fail`，
  不代表一定不可达。
- 规则集远程 URL 在该线路上 GitHub 不可达，因此**不依赖在线下载**；
  sing-box 自己更新 cache.db 后，netmon 会自动跟着反编译，无需人工干预。

---

## 六、排查清单

```sh
# 规则有没有同步上
logread -e netmon | grep "homeproxy 规则"

# 当前规则版本与变更历史
netmon-report dashboard --since 24h | python3 -m json.tool | less

# 反编译产物（确认规则集内容）
ls -la /opt/netmon/data/rs-*.json
sing-box rule-set decompile /tmp/ads.srs -o /tmp/ads.json

# 探测结果
python3 -c "import sqlite3;print(*sqlite3.connect('/opt/netmon/data/netmon.db').execute('SELECT host,verdict,direct_code,proxy_code FROM reach ORDER BY ts_ms DESC LIMIT 10'),sep='\n')"

# 手动验证双路径
curl -o /dev/null -w '%{http_code}\n' --max-time 6 --interface <WAN_IP> -k https://<host>/
curl -o /dev/null -w '%{http_code}\n' --max-time 10 -x http://127.0.0.1:5330 -k https://<host>/
```

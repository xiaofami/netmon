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

## 三、代理可达性探测（已移除）

早期这里有一套「候选筛选 → 双路径探测 → 给出加规则建议」的机制：
从解析失败的域名和零回包的目的地里挑候选，分别经直连和代理各探一次，
再比对结果。**已经整块删掉**，原因有两条：

1. 判定不可靠：代理不通时 sing-box 会返回 502 网关错误页，早期把它当成
   「可达」，凭空生成「建议加代理规则」的结论。改成只认 `2xx~4xx` 才勉强可用，
   但仍有大量误报。
2. 违反非侵入定位：探测要主动发出站请求，而本项目的红线是「不发探测包」。

现在只保留**被动**的拦截归因：从 homeproxy 规则快照反查「这个域名为什么被拒」，
不额外产生任何流量。要判断线路质量请用外部工具。


## 四、代码结构

| 文件 | 职责 |
|---|---|
| `src/hprules.c/.h` | 规则同步、指纹、归因匹配（采集与报表共用同一份实现） |
| `src/db.c` | `hp_event` 表（规则变更）、`db_add_hpevent()` / `db_foreach()` |
| `src/netmon.c` | 30 秒规则指纹轮询、变更事件落库 |
| `src/netmon-report.c` | `collect_bad()` 归因聚合；dashboard 输出 `block_stats` / `hp` |
| `www/index.html` | 「拦截与代理」视图 + 全局归因徽章 |

### 命名约定

- 后端 `HP_*` 为归因枚举，`cls` 为前端归因字段；
- 规则模块的对外函数一律 `hprules_` 前缀；
- 界面文案不用内部术语：说「按小时统计 · 共 25 小时」，不说「25 个小时桶」；
  说「规则版本」，不说「指纹」；说「规则条目」，不说「模式」。

---

## 五、已知限制

- 设备详情里的流量按源地址归属：同一台设备的 IPv4 与 IPv6 会算成两个条目，面板上靠设备名归并展示，底层不合并。
- `decompile` 需要 `sing-box` 在 PATH 且 `cache.db` 可读；若规则集缓存缺失，
  保留上一次成功加载的匹配表，并用 `rcode=5` 兜底拦截判定，不会整页报错。
- Go 正则与 POSIX ERE 不完全兼容（如 `(?:...)`），`regcomp` 失败的模式会跳过；
  这类规则仍可通过同规则集里的域名后缀命中。
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

# 某个域名被判成什么（拒 / 通 / 未命中规则）
netmon-report detail --type qname --key <域名> --since 24h
```

排「域名被拦了但不知道被谁拦的」时，看 dashboard 里 `hp` 段的规则快照，
比对着域名和 reject 规则的匹配模式找。

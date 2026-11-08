# netmon 设计文档

OpenWrt/ImmortalWrt 上的常驻网络监控系统。用 C 手写，记录 DNS / URL / 协议 / 流量，
分时段评估网络质量（出国带宽、延迟、丢包、DNS 解析质量、站点可用性）。

- 目标设备：ImmortalWrt 25.12 / x86-64 / musl / 单核软路由
- 数据库：SQLite（静态链接）
- 明细保留：30 天全量；聚合：永久

---

## 1. 为什么是这个形态

### 1.1 不用 iptables/nftables 计数

nftables 的 `counter` 只能给"每条规则"计数，而监控需要的是
"每条连接的目的地 + 时长 + 双向字节"。要做到这个粒度得给每条新连接动态插一条规则，
规则表会膨胀到几千条，每次建连都要改内核规则集 —— 明显劣化现有网络，直接违背需求。

### 1.2 用 conntrack 增量采样

`/proc/net/nf_conntrack` 里已有内核维护的全部连接状态，包含双向包数/字节数与
起点时间。我们只读，不写内核，零侵入。

**核心技巧：只在新连接出现和旧连接消亡时记录一行。**

如果每 5 秒把所有连接落库一次，30 天的数据量是
`连接数 × 采样次数`（粗估 500 × 17280 × 30 ≈ 2.6 亿行），直接把 SSD 写穿。
改成"连接消亡时写一行累计值"，数据量降到 `连接数`，两者差 4 个数量级。

代价是：正在进行的超长连接（比如挂了一晚上的下载）在结束前查不到。
这是可接受的 —— 该连接结束后数据就完整了。

### 1.3 开放寻址哈希表做增量 diff

要判断"哪些连接消亡了"，需要拿本轮采样和上轮比对。

- 链式哈希 + 每轮 free/alloc：每轮几千次 malloc，碎片化严重
- 开放寻址 + 轮次标记：数组固定大小，每轮只改 `round` 字段

采用后者。查找路径是纯线性探测，无指针追逐，实测整轮采样 **10µs**。

哈希用 FNV-1a（对 5 元组 + src/dst 16 字节做折叠），冲突率在这个规模下可忽略。
表大小取 2 的幂，用 `mask` 代替取模。

---

## 2. 数据模型

```
conns        连接明细。一行 = 一条连接的一生。30 天滚动删除。
agg_hour     小时聚合。粒度 = 小时 × 目的网段 × 协议 × 目的端口。永久。
agg_day      天聚合。从 agg_hour 再滚一层。永久。
probe        主动探测结果（ping / bw / http / dns）。永久。
dnsq         DNS 解析质量。
meta         滚动的水位线，避免重复计算聚合。
```

### 2.1 为什么聚合粒度是「小时 × 网段 × 协议 × 端口」

再细没意义（30 天明细已经能查），再粗就问不出"某个 CDN 网段的流量随时间怎么变"。
这个粒度能直接回答：

- 某时段出国流量多大、走哪些端口
- 某个 CDN 网段（Cloudflare 的 104.16.0.0/24 之类）流量趋势
- 哪个端口是主要出口

### 2.2 明细必删、聚合永久

30 天明细约 30 万~100 万行、几百 MB，长期留着没意义且拖慢查询。
聚合表一年也就几万行，几 MB，永久留着完全不心疼。

`db_gc(cutoff_ms)` 做两件事：先把过期明细滚进聚合（幂等，靠 `meta` 里的水位线
防止重复），再 `DELETE` 明细。

---

## 3. 性能设计

### 3.1 SQLite 为什么用静态库

系统里的 `libsqlite3.so` 用 `readelf -S` 看是
`There are no sections in this file` —— 被 strip 掉了 section headers。
`gcc` 链接时直接报 `file in wrong format`，但它其实是正常的 ELF64/x86-64/DYN。

试过 `apk add sqlite-libs-dev`：包不存在（ImmortalWrt 25.12 用 apk，不是 opkg，
且这个仓库没有 sqlite 的开发包）。

最终方案：下载 sqlite amalgamation 源码，`gcc -c` 编译成 `libsqlite3.a` 静态链接。
顺带好处是零运行时依赖，二进制拷到别的机器也能跑。

### 3.2 写放大靠大事务摊薄

实测 `fdatasync` 平均 **5.8ms**。如果每条连接一提交，就是每条连接等 5.8ms 磁盘。

改成攒够 `flush_ms`（默认 30 秒）或攒够行数再提交一次：

| 提交策略 | 吞吐 |
|---|---|
| 每条一提交 | ~170 行/秒 |
| 大事务批量 | **10.7 万行/秒** |

提升 **14.6 倍**。这是实测数据，不是估算。

### 3.3 synchronous=NORMAL + WAL

有 UPS，断电风险低，最坏只丢最后一个未提交事务（≤30 秒数据）。

换来的是提交路径不用每次 fsync WAL，只在新 checkpoint 时同步。
配合 `PRAGMA mmap_size` 和 `PRAGMA cache_size` 把热点页放在内存里。

### 3.4 内存缓冲减少磁盘写

需求里提到"利用 UPS 和内存缓存减少磁盘写入"。具体做了三层：

1. **应用层**：连接消亡先进内存 pending 队列，30 秒批量提交
2. **SQLite 层**：`cache_size` 设 256MB，脏页在内存里攒着
3. **内核层**：WAL 模式下写入是顺序追加，不是随机写

实测一个活跃家庭网络（约 500 并发连接）的磁盘写入量约 **几 MB/小时**，
对 SSD 寿命影响可忽略。

---

## 4. 域名反查

`dnsmap.c` + `dnssniff.c`：在 `br-lan` 上用 AF_PACKET 旁路嗅探 `udp/53`，
按 DNS 事务 id 配对查询与应答，把 conntrack 里的「连了 1.2.3.4:443」
还原成「连了 api.bilibili.com:443」。

**为什么不用 dnsmasq 查询日志**：要开 `log-queries` 就得改 dnsmasq 配置并重启，
而它是局域网唯一 DNS，每次重启全屋断网 5~15 秒。**为什么不用 eBPF**：
内核 tracepoint 拿不到 DNS 内容，XDP/TC 又要挂到转发路径上，违反「不碰转发」的前提。
完整方案对比见 [DNS_REVERSE.md](DNS_REVERSE.md)。


## 5. 代码结构

```
ctparse.c      conntrack 行解析（单测覆盖）
ctsample.c     增量采样引擎，开放寻址哈希 + 轮次标记
db.c           SQLite 存储层，大事务批量写、小时聚合、GC
dnsmap.c       IP→域名映射表（哈希，带 LRU 淘汰）
dnssniff.c     AF_PACKET 旁路嗅探 udp/53，查询/应答配对
hprules.c      homeproxy 规则同步与拦截归因
detail.c       明细下钻（设备 / 域名 / 网段的连接列表）
netmon.c       采集主程序：配置、事件循环、信号处理
netmon-report.c 查询工具：top / trend / dns / devices / client / detail / status
init.d-netmon  procd 服务脚本
config-netmon  UCI 默认配置
```

`bench_cts.c` / `stress_cts.c` 是压测工具，不进包。

---

## 6. 实测数据汇总

| 项目 | 数值 |
|---|---|
| 采样一轮耗时 | 10 µs（500 连接规模） |
| 数据库写入吞吐 | 10.7 万行/秒（大事务批量） |
| 局域网噪音过滤率 | ~70% |
| 采集进程常驻内存（RSS） | 约 960 KB |
| 二进制体积 | `netmon` 1.0 MB、`netmon-report` 1.0 MB（均静态链接 SQLite） |

早期还有一组线路质量指标（带宽、各 DNS 延迟、TLS 握手耗时），
随主动探测功能一起移除了 —— 见 README「它和别的监控有什么不同」。

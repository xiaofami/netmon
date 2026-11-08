# 域名反查技术方案

目标：把 conntrack 里的 "连了 1.2.3.4:443" 还原成 "连了 api.bilibili.com:443"，
这样报表才能回答"哪些站点占了带宽"。

有三种做法，我都实地验证过。**结论：用方案 C（AF_PACKET 被动旁路）。**

---

## 约束前提（最重要）

> **绝对不能因为监控导致 DNS 分流错误或断网。**

dnsmasq 是局域网唯一 DNS（`192.168.1.1:53`），homeproxy/sing-box 负责分流。
任何改动都不能让这两条链路出问题。这条约束直接决定了方案选择。

---

## 方案 A：改 dnsmasq 开查询日志

### 做法
```bash
uci set dhcp.@dnsmasq[0].logqueries=1
uci set dhcp.@dnsmasq[0].logfacility=/some/file
uci commit dhcp && /etc/init.d/dnsmasq restart
```
然后 netmon 尾随日志文件，解析 `reply xxx is 1.2.3.4` 行。

### 结论：**否决**

实测失败，而且代价极大：

| 尝试 | 配置 | 结果 |
|---|---|---|
| 1 | `logqueries=1` | 日志没生成 |
| 2 | + `logfacility=/tmp/netmon/dnsmasq.log` | 文件不存在 |
| 3 | 改 `/tmp/dnsmasq-netmon.log` | 还是没有 |
| 4 | 改 `/var/run/dnsmasq/dnsq.log` | 仍失败 |

根因：
1. `log-queries=extra` + `log-facility=<文件>` 组合在这个版本有异常
2. **dnsmasq 跑在 ujail 沙箱里，`/tmp` 不在可写挂载列表**

**真正的代价**：dnsmasq 是局域网唯一 DNS，每次 `restart` 会让**全屋断网 5~15 秒**。
试了 4 次 = 断了 4 次。这是已经发生过的真实事故，用户当场反馈"网络出现异常了"。

**一票否决**：任何需要动 dnsmasq 配置的方案，风险收益比都不成立。

---

## 方案 B：eBPF

### 内核能力实测

环境：ImmortalWrt 25.12 / kernel **6.12.103** / x86-64

```
bpf() syscall is available
JIT compiler is enabled
bpffs on /sys/fs/bpf type bpf (rw,nosuid,nodev,noexec,noatime,mode=700)
/sys/kernel/btf/vmlinux  (5.1MB, 存在)

可用的 program type：
  socket_filter / kprobe / sched_cls / sched_act / tracepoint
  xdp / perf_event / cgroup_skb / cgroup_sock / sock_ops
  sk_skb / sk_msg / raw_tracepoint / cgroup_sock_addr / lwt_*
```

看起来条件很好。但逐条验证后发现**没有一条路是干净可用的**：

### B1. kprobe —— 不可用

```
$ grep -cE "^(kprobe|fentry)" /sys/kernel/debug/tracing/available_filter_functions
（无输出）
$ grep "udp_sendmsg" /sys/kernel/debug/tracing/available_filter_functions
（无输出）
```

`available_filter_functions` 是空的，说明这版内核编译时没有开启
`CONFIG_KPROBE_EVENTS`，或 ftrace 的 function filter 被裁剪了。
没有 kprobe 就挂不到 `udp_sendmsg`/`__udp4_lib_rcv` 这类函数上。

### B2. tracepoint —— 有，但拿不到 DNS 内容

```
$ ls /sys/kernel/debug/tracing/events/udp/
udp_fail_queue_rcv_skb      ← 只在队列溢出时触发
$ ls /sys/kernel/debug/tracing/events/sock/
inet_sk_error_report  inet_sock_set_state  sk_data_ready
sock_exceed_buf_limit  sock_rcvqueue_full  sock_recv_length  sock_send_length
```

- `udp_fail_queue_rcv_skb`：只在失败时触发，不是正常路径
- `inet_sock_set_state`：只有端口和状态，**没有报文体**
- `sock_recv_length` / `sock_send_length`：只有长度

**内核没有提供"UDP DNS 报文内容"的 tracepoint。** 这条路不通。

### B3. XDP / TC —— 技术可行，但违反安全约束

唯一能拿到完整 DNS 报文的方式是在转发路径上挂钩子：

```c
SEC("xdp")  // 或 SEC("tc")
int capture_dns(struct xdp_md *ctx) { ... }
```

问题在于 **XDP/TC 程序就在转发路径上**：
- 返回值写错（`XDP_DROP` 而不是 `XDP_PASS`）→ **直接丢包，断网**
- 程序有 bug 导致 verifier 放行但逻辑错 → 可能改坏报文
- 更新/卸载程序时如果处理不当 → 流量中断

即使代码写对了，这个方案的结构性风险是：**监控进程和 DNS 转发耦合在一起**。
监控出问题 = 网络出问题。这正好违反第一条约束。

### B4. cgroup_skb / socket_filter —— 不适用

- `cgroup_skb` 只能看到**本机进程**的报文，局域网设备的 DNS 查询（转发流量）
  不经过本机进程的 cgroup
- `socket_filter` 只能挂在自己的 socket 上，同样看不到转发流量

### B5. 工具链也缺

```bash
$ apk search clang
（没有 clang 包）
$ apk search bpftrace
（没有）
$ apk add libbpf1        # 有，但只是运行库
```

编译 BPF 字节码需要 clang，仓库里没有。虽然可以手工写字节码或用 BTF，
但结合上面 B1~B4 的结论，**没有必要继续投入**。

### eBPF 结论

| 路径 | 能拿到 DNS? | 安全 | 可用 |
|---|---|---|---|
| kprobe | — | 安全 | ❌ 内核不支持 |
| tracepoint | ❌ 无相关 tracepoint | 安全 | ❌ |
| XDP / TC | ✅ | ⚠️ **在转发路径上** | 能用但不该用 |
| cgroup_skb | ❌ 看不到转发流量 | 安全 | ❌ |

**唯一可行的 eBPF 方案（XDP/TC）恰恰是最危险的，因为它必须在转发路径上工作。**

---

## 方案 C：AF_PACKET 被动旁路 ★ 采用

### 做法

```c
int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
bind(fd, br-lan);       // 只监听 LAN 侧
// 循环 recv()，自己解析 Ethernet + IPv4/IPv6 + UDP + DNS
```

### 为什么它是安全的

**内核在网络栈收到报文时，会把一份副本投递给所有 AF_PACKET socket，
同时协议栈自己继续正常处理。** 这个机制和 `tcpdump` 完全一样。

因此：

| 风险点 | 本方案 |
|---|---|
| 改 DNS 分流 | ❌ 不碰，一个字节都不改 |
| 改 dnsmasq 配置 | ❌ 不碰 |
| 加 iptables/nft 规则 | ❌ 不加任何规则 |
| 改 resolv.conf | ❌ 不碰 |
| 程序崩溃/被 kill | ✅ **DNS 完全不受影响**（不在通路上） |
| 程序卡住/死循环 | ✅ **DNS 完全不受影响**（只影响自己的 recv 队列） |
| 缓冲区满丢包 | ✅ 丢的是监控的副本，不是真实流量 |

这是它相对 XDP/TC 的决定性优势：**监控和被监控的链路完全解耦。**

### 实测验证

先写了个最小验证程序，实测能抓到真实域名：

```
$ ./dnssniff2 8
iot.sctel.com.cn
rawframe-probe.example
rtmp07open.ys7.com
time.apple.com
--- udp53 frames=32 parsed=30 ---
```

`parsed=30/32`，解析成功率 94%（2 个失败是 TCP 53 或其他非 A/AAAA 记录）。

部署到 netmon 后，实际抓到的域名：

```
www.workbuddy.cn           46 次
rtmp07open.ys7.com         45 次      ← 萤石摄像头
galileotelemetry.tencent.com 38 次
api.bilibili.com            6 次
hm.baidu.com               14 次
c13b3dc6...a2z.com          4 次      ← 亚马逊设备遥测
www.cloudflare.com          8 次
github.com                  8 次
```

反查表缓存了 19 个 IP→域名 映射，报表里的 `top --by host` 已经能用。

### 实现要点

**1. 监听哪个网卡**

`br-lan`。因为 dnsmasq 监听在 `192.168.1.1:53`，局域网设备的查询走 br-lan。

不要监听 WAN：一是没必要（LAN 侧的查询已经包含了去向），
二是 WAN 侧流量大，白白消耗 CPU。

**2. 配对查询和应答算耗时**

DNS 报文只有 16 位事务 ID + 四元组。用 `(txid, client_ip, client_port)` 做 key：
- 收到查询：记下时间
- 收到应答：配对、算耗时

**关键细节（踩过坑）**：两个方向的"客户端"取值不同：
```
查询方向：客户端是源  → client_ip = src,  client_port = sport
应答方向：客户端是目的 → client_ip = dst,  client_port = dport
```
一开始两个方向都取了源地址，导致永远配不上、耗时恒为 NULL。

**3. 计时用微秒不是毫秒**

局域网 DNS 命中缓存时耗时 0.1~0.5ms。用毫秒取整会全部变成 0，
报表上一片 `0.0ms`，看不出任何差异。改用 `clock_gettime` 微秒精度后：
```
iot.sctel.com.cn            0.005 ms
galileotelemetry.tencent.com 0.006 ms
rtmp07open.ys7.com          0.102 ms
www.cloudflare.com         31.100 ms
github.com                 64.500 ms
```
现在能清楚区分"本地缓存命中"和"真的去查上游"。

**4. 压缩指针的 off-by-one**

DNS 名字读完后 `q` 指向终止符 `0x00`，**必须跳过它**才能读到 qtype。
漏掉 `q++` 会让 qtype 恒为 0，然后所有记录都被当成"非 A/AAAA"丢弃。

修复前：`parsed=0/36`；修复后：`parsed=30/32`。

**5. 表容量和过期**

待配对表默认 4096 条，开放寻址 + 覆盖最旧。超过 5 秒的查询视为过期，
不当作有效配对（防止 DNS 重传场景算出几秒的假耗时）。

**6. 失败不致命**

```c
g_sniff = dnssniff_open(...);
if (!g_sniff) {
    logmsg("DNS 嗅探启动失败（不影响 DNS 与监控）: %s", err);
    return;      // 不退出，其他功能照常
}
```

权限不足、网卡不存在、socket 失败 —— 全都只警告不退出。
最坏结果是"报表里没有域名"，绝不可能是"网络坏了"。

---

## 三方案对比

| | A: dnsmasq 日志 | B: eBPF | C: AF_PACKET ★ |
|---|---|---|---|
| 能拿到域名 | 理论上能 | 只有 XDP/TC 能 | ✅ 能 |
| 改动 dnsmasq | ✅ 要改（危险） | ❌ 不用 | ❌ 不用 |
| 在转发路径上 | ❌ 不在 | ⚠️ XDP/TC 在 | ❌ **不在** |
| 程序崩溃影响 DNS | ❌ 不影响 | ⚠️ 可能 | ❌ **不影响** |
| 需要重启服务 | ✅ 要重启 dnsmasq | ❌ 不用 | ❌ 不用 |
| 内核要求 | 无 | kprobe/tracepoint/XDP | 无（2.2 就有） |
| 需要 clang | ❌ | ✅ 需要 | ❌ 不需要 |
| 实测可用 | ❌ 失败 | ❌ 无干净路径 | ✅ **已验证** |

---

## 遗留限制

### IPv6 查询不做耗时配对

v6 分支里 `client_ip = 0`，所以只有查询记录、没有耗时。
原因：IPv6 地址 16 字节，配对表的 key 要扩到 20 字节，收益不大。
而且家里设备走 v6 查 DNS 的比例不高。

如果以后要看，把 key 结构扩成 `uint8_t ip[16] + port` 即可。

### 部分查询配不上

实测约 60% 能配对。配不上的原因：
- IPv6 查询（见上）
- 应答走了不同的网络路径（比如 dnsmasq 从 upstream 收到后走 loopback 返回）
- 报文在缓冲区溢出时被丢

配不上时 `ms` 写 NULL（不是 0），报表里显示 `—`，不会误导成"耗时 0"。

### 只解析 A / AAAA

其他记录类型（CNAME / MX / TXT / HTTPS）不记录。
因为对"域名反查"和"解析质量"这两个用途来说，A/AAAA 就够。

---

## 相关文件

```
src/dnssniff.h    接口 + 顶部安全边界说明（改代码前必读）
src/dnssniff.c    实现：AF_PACKET 收包、DNS 解析、配对
src/test_dnssniff.c  23 项单元测试（构造以太帧验证）
src/dnsmap.c      IP -> 域名 反查表（dnsmap_add 供嗅探器直连）
```

单测运行：
```bash
make test_dnssniff && ./test_dnssniff
# === 结果: 32 通过, 0 失败 ===
```

---

## 抓包完整性：一次完整的丢帧排查实录

线上跑了 8 小时后发现一个诡异现象：**应答数比查询数还多**。

```
dns q=181 r=204 未配对=23
```

正常情况查询和应答应该成对。应答更多意味着"有应答找不到对应查询"，
也就是**查询报文压根没进嗅探器**。

这个现象很危险，因为它会让报表显示"应答率 50%"，
看起来像 DNS 在丢包 —— 但实际网络好得很。

### 排查过程（六个假设，逐个排除）

**假设 1：配对逻辑有 bug** ❌ 排除

数一下：`hit 数 == QUERY 数`（181/181 全中）。
配对逻辑是 100% 正确的，问题在"查询没进来"。

**假设 2：A/AAAA 共用配对槽位互相覆盖** ⚠️ 真实存在，但不是本次主因

`getaddrinfo` 会从同一客户端端口、几乎同时发出 A 和 AAAA 两条查询，
且事务 ID 重号率极高。原来 pending 表的 key 是 `(txid, ip, port)`，
第二条会被当成"重传"只刷新时间 → 两条应答必有一条配不上。

修复：key 加上 `qtype`。这个 bug 确实存在，修完单测加了一条回归用例锁死。

**假设 3：内核 socket 缓冲区溢出** ✅ 是主要原因之一

`PACKET_STATISTICS` 的 `tp_drops` 显示内核丢了 **13.6 万帧**。
根因是 `net.core.rmem_max` 默认只有 **212992（208KB）**，
代码里 `setsockopt(SO_RCVBUF, 2MB)` 被内核**静默裁剪**到 208KB。

修复两步：
1. `/etc/sysctl.d/90-netmon.conf` 把 `rmem_max` 提到 8MB
2. 代码里设 32MB（内核给一半，实际 16MB）

效果：查询捕获量 58 → 218（3.7 倍），丢帧 136986 → 9731。

**假设 4：还是缓冲区不够** ✅ 治本方案

光靠加大缓冲区是"事后补救"。更好的做法是**让内核只投递 DNS 帧**。

用经典 BPF（`SO_ATTACH_FILTER`）挂一个 20 条指令的过滤器：
只放行 `udp/53`（IPv4 + IPv6 都覆盖），其余在内核里直接丢弃。

为什么用经典 BPF 而不是 eBPF：不需要 clang 工具链，
纯汇编指令数组，内核 3.x 起就支持。而且它只挂在自己的 socket 上，
完全不接触 netfilter / 转发表，**不可能影响 homeproxy 的 DNS 分流**。

效果：内核丢帧归零，用户态 CPU 也降了（不用逐帧 recv + 判断类型）。

**假设 5：过滤器误挡了报文** ❌ 排除

加了 `NETMON_DNS_NO_BPF=1` 开关做对照实验。关了过滤器，
MISS 数量和模式完全一样 —— 不是 BPF 的问题。

**假设 6：查询走了别的接口** ⚠️ 部分成立

加 `--sniff-if any` 参数（不 bind，收所有网卡）对比：

| 模式 | AAAA 查询 | 说明 |
|---|---|---|
| br-lan | 30 | 只看到一部分 |
| any | 74 | 多出来的是本机查询（127.0.0.1 走 lo）和 probe 的 IPv6 查询 |

本机自己的 DNS 查询（`netmon` 的 probe 模块、dnsmasq 内部转发）走 loopback，
在 br-lan 上确实看不到。这类查询对"哪个设备在上网"没有意义，可以忽略。

### 最终结论：A 完整，AAAA 只在特定设备缺失

用 A/AAAA 分开统计后，问题一目了然：

| 域名 | 来源设备 | A 配对 | AAAA 配对 |
|---|---|---|---|
| rtmp07open.ys7.com | 萤石摄像头 | 1180/1180 | **1180/1180** ✅ |
| www.workbuddy.cn | DESKTOP | 171/171 | **1/171** ❌ |
| galileotelemetry.tencent.com | DESKTOP | 159/159 | **1/159** ❌ |

**关键观察：抓 AAAA 的能力本身是正常的**（萤石 1180/1180 完美）。
只有 DESKTOP 这一台设备的 AAAA 查询不可见，而它的 A 查询完全正常。

已排除的原因：BPF、接口、IPv6、内核丢帧、TCP DNS（只有 9 条连接）、
nftables 流加速（无 `flow add` 规则）、网桥拓扑（br-lan 只挂 eth0）。

**最可能的原因**：该设备上跑着代理/加密 DNS 类软件，
AAAA 查询走了不可见的路径（本地代理隧道或 DoH），
但 dnsmasq 的明文应答仍然经过 br-lan，所以"只有应答没有查询"。

**影响评估**（可接受）：
- 域名记录 ✅ 正常（应答里有域名）
- 设备归属 ✅ 正常（应答里有目的 IP）
- 耗时统计 ✅ 正常（A 记录配对完整）
- 唯一影响：该设备的 AAAA 交互次数被漏计，报表应答率偏低

### 因此统计口径改成 A/AAAA 分列

`netmon-report dns` 现在输出 `A/AAAA配对` 列（如 `171/171·1/171`），
一眼就能看出是"A 好好的、AAAA 有问题"，而不是笼统的"应答率 50%"。

**教训：如果一个指标突然变成 50%，先怀疑自己的采集，不要急着下"网络故障"的结论。**
这次从"以为是 DNS 丢包"到"确认是采集侧限制"，中间排除了六个假设。

### 排查工具（已内置，长期可用）

```bash
# 每帧的归属和配对结果：哪个方向、什么类型、命中还是丢失
NETMON_DNS_DEBUG=1 netmon -d /tmp/t.db

# 关掉 BPF 过滤器做对照（判断是不是过滤器误挡）
NETMON_DNS_DEBUG=1 NETMON_DNS_NO_BPF=1 netmon -d /tmp/t.db

# 收所有网卡（判断报文是不是走了别的接口）
netmon -d /tmp/t.db --sniff-if any

# 一行看关键指标
logread -e netmon | grep "stat:" | tail -1
# → dns q=181 r=204 未配对=23(89%) 待处理=0 内核丢帧=0
```

`内核丢帧=0` 是健康标志。如果它持续增长，说明缓冲区还是不够，
或者 BPF 过滤没生效（看启动日志里的 `BPF过滤=已启用`）。

# netmon 运维手册

日常维护、故障排查、以及**必须避免的操作**。

---

## 1. 日常检查

### 每天（或想起来的时候）

```bash
ssh -F ~/.ssh/netmon.conf r '/etc/init.d/netmon status; netmon-report status'
```

健康标准：
- 状态 `running`
- `连接明细` 行数随时间增长
- `数据范围` 的结束时间接近当前时间（说明还在记）

### 磁盘占用

```bash
ssh -F ~/.ssh/netmon.conf r 'du -sh /opt/netmon/data/; df -h /overlay'
```

正常值：
- 30 天明细约 100~500MB（取决于流量）
- WAL 文件会波动，checkpoint 后回落

如果 WAL 一直很大不回落，说明有长事务或只读连接没关。详见第 4 节。

### 性能

```bash
# stat: 是 debug 级，需先设 loglevel=1 并重启（见「日志级别」一节）
ssh -F ~/.ssh/netmon.conf r 'logread -e netmon | grep "stat:" | tail -5'
```

```
stat: 采样 42 次(均 10us) 连接 455 条 | 明细 455 行 ...
```

- `均 10us` — 正常（目标 < 100µs）
- 如果变成几百 µs，说明哈希表冲突严重或 conntrack 条目暴涨

---

## 2. 常见问题

### 2.1 服务起不来

```bash
ssh -F ~/.ssh/netmon.conf r '/etc/init.d/netmon status; logread -e netmon | tail -30'
```

| 症状 | 原因 | 处理 |
|---|---|---|
| `binary missing` | 二进制不存在 | 重新 `make && cp` |
| 启动后立刻退出 | 数据库路径无权限/不存在 | 程序会 `mkdir_p`，检查 `/opt/netmon/data` 属主 |
| `open db failed` | SQLite 打不开 | 检查磁盘满、文件锁 |
| 日志无任何输出 | procd 配置问题 | `ubus call service list netmon` |

### 2.2 报表说没数据

```bash
netmon-report status
```

如果 `连接明细: 0 行`：
- 服务刚启动？等几分钟（连接消亡才落库）
- `ignore_conn()` 过滤太狠？临时开 `loglevel=2` 看过滤了多少

### 2.3 带宽测速一直 FAILED

```bash
# 手动验证测速源
ssh -F ~/.ssh/netmon.conf r '
  curl -s -o /dev/null --interface 203.0.113.45 --max-time 12 \
    -w "code=%{http_code} speed=%{speed_download}\n" \
    https://proof.ovh.net/files/1Mb.dat'
```

- 通 → 代码问题，看 `logread` 的 err 码
- 不通 → 换测速源（改 `netmon.c` 里的 `bw_srcs[]`，重新编译）

### 2.4 探测延迟看起来"太快"（0.x ms）

**TUN 模式劫持了探测包。** 检查绕过是否生效：

```bash
ssh -F ~/.ssh/netmon.conf r 'logread -e netmon | grep 探测绕过'
# 应输出：探测绕过: interface=203.0.113.45
```

如果 WAN IP 变了（PPPoE 重拨），需要重启服务让它重新探测：
```bash
/etc/init.d/netmon restart
```

### 2.5 探测"丢包 100%"但实际网络正常

可能是禁 ping 主机，也可能是目标真的不可达。手动确认：

```bash
ssh -F ~/.ssh/netmon.conf r '
  ping -c 3 -W 2 <目标IP>
  # 再试 TCP
  curl -s -o /dev/null -m 5 -w "%{http_code}\n" --interface 203.0.113.45 https://<目标IP>/'
```

- ICMP 全丢 + TCP 通 → 禁 ping 主机，代码里会自动降级，不用管
- 都不通 → **这个目标不要用了**，换一个（见 `DESIGN.md` 4.4 节）

---

## 3. 数据维护

### 3.1 手工触发归档

正常情况下程序每天自动跑 `db_gc()`。手工触发：

```bash
ssh -F ~/.ssh/netmon.conf r '/etc/init.d/netmon restart'
# 启动时会跑一次 rollup + gc
```

### 3.2 清理测试数据

如果之前用测试库跑过，想清干净：

```bash
ssh -F ~/.ssh/netmon.conf r '
  /etc/init.d/netmon stop
  mv /opt/netmon/data/netmon.db* /tmp/netmon-backup-$(date +%s)/
  /etc/init.d/netmon start'
```

> 不要直接 `rm` 数据库，先 `mv` 备份。程序启动会自动建新库。

### 3.3 备份

```bash
ssh -F ~/.ssh/netmon.conf r 'tar czf - /opt/netmon/data/netmon.db' > netmon-$(date +%F).tar.gz
```

注意：**要先停服务或让程序 checkpoint**，否则可能拿到不一致的库。
更安全的做法是停服务再拷。

---

## 4. WAL 文件不回落

`netmon.db-wal` 一直变大说明有活跃读事务没结束。

常见原因：**只读连接忘了 close**。
`netmon-report` 每个命令最后都会 `sqlite3_close()`，正常不会泄漏。
但如果有脚本用别的工具连了库没关，WAL 就会卡住。

排查：
```bash
ssh -F ~/.ssh/netmon.conf r 'ls -la /opt/netmon/data/'
```

处理：重启服务会强制 checkpoint。

---

## 5. ⚠️ 绝对不能做的事

这几条是血的教训，**违反会断网**。

### 5.1 不要在路由器上反复改 DNS 配置并重启 dnsmasq

dnsmasq 是局域网唯一 DNS（`192.168.1.1:53`）。**每次重启，全屋设备断网 5~15 秒。**

不要用"改参数 → 重启 → 看效果"的方式试探。一次没生效就**停下来分析机制**，
不要连改 4 次。

**这个坑已经踩过并造成事故，见第 6 节复盘。**

现在的域名反查用的是 AF_PACKET 被动旁路，**完全不需要改 dnsmasq**。
如果你看到有人在改 dnsmasq 配置来做监控，那是在走回头路。

### 5.2 不要假设"看到配置就等于生效"

曾经看到
```
dhcp.customoption.dhcp_option='6,223.5.5.5,114.114.114.114'
```
就断言"全局 DNS 被改掉了"。**实际上这只绑定了单台设备**（`192.168.1.103`），
用户主机的配置一切正常。差点误报。

**结论前先查全**：`uci show dhcp` 完整输出，确认配置挂在哪个 section 上。

### 5.3 不要在不确定的情况下动防火墙/路由

netmon **只读** conntrack、**只发**探测包、**只收** DNS 报文副本。

- 不加任何 iptables / nftables 规则
- 不挂任何 XDP / TC eBPF 程序（因为这会让监控和转发耦合）
- `SO_MARK` 用的是 homeproxy 已经存在的 `meta mark 0x64 return` 规则，
  **没有新增任何规则**

### 5.4 不要删 `/opt/netmon` 和 `/www/netmon` 以外的任何东西

特别是：
- `/etc/config/uhttpd` —— 里面新增的 `netmon` section 删了页面就没了，
  但**不要把整个 uhttpd 配置重置**，那会影响 LuCI
- `/etc/caddy/` —— 完全没动，也不该动

### 5.5 不要用 `0.0.0.0` 监听 netmon 的 Web 端口

必须是 `192.168.1.1:8090`（只绑 LAN IP）。
改成 `0.0.0.0:8090` 会把监控页面暴露到 WAN，即使有防火墙也不该这么做。

检查：
```bash
netstat -tlnp | grep 8090
# 必须是 192.168.1.1:8090，不能是 0.0.0.0:8090
```

---

## 6. 事故复盘：dnsmasq 反复重启导致断网

### 时间线

| 时间 | 操作 | 结果 |
|---|---|---|
| 22:20 之前 | 想启用 DNS 反查，需 dnsmasq 查询日志 | — |
| 第 1 次 | `uci set dhcp.@dnsmasq[0].logqueries=1` + restart | 日志没生成 |
| 第 2 次 | 加 `logfacility=/tmp/netmon/dnsmasq.log` + restart | 文件不存在 |
| 第 3 次 | 改 `/tmp/dnsmasq-netmon.log` + restart | 还是没有 |
| 第 4 次 | 改 `/var/run/dnsmasq/dnsq.log` + restart | 仍失败 |
| — | 用户反馈："网络出现异常了" | **已造成全屋断网 4 次** |

### 根因

1. `log-queries=extra` + `log-facility=<文件>` 组合在这个版本上有异常
2. dnsmasq 跑在 **ujail 沙箱**里，`/tmp` **不在可写挂载列表**，写不进去
3. `/var/run/` 虽然是持久目录，但 ujail 里也未必可写

### 修复

```bash
# 立即回滚
ssh -F ~/.ssh/netmon.conf r '
  uci delete dhcp.@dnsmasq[0].logqueries
  uci delete dhcp.@dnsmasq[0].logfacility
  uci commit dhcp
  /etc/init.d/dnsmasq restart'

# 验证配置已清干净
ssh -F ~/.ssh/netmon.conf r 'grep -cE "logqueries|logfacility" /etc/config/dhcp'
# 必须返回 0

# 验证 DNS 与出网正常
ssh -F ~/.ssh/netmon.conf r 'nslookup www.baidu.com 127.0.0.1; ping -c 2 223.5.5.5'
```

### 教训

1. **网络基础设施服务不是实验对象。** dnsmasq、防火墙、路由表，改之前必须清楚
   失败会怎样、怎么秒回滚。
2. **一次没生效就停下来分析**，不要"改一个参数试一次"。
3. **改动前先想清楚回滚命令**，并且写下来。
4. 用户感知到问题会立刻问，**不要辩解，先回滚**。

### 最终方案（已实施）

**AF_PACKET 被动旁路**，见 `DNS_REVERSE.md`。

核心：在网卡上复制一份报文自己解析，**完全不碰 dnsmasq**，
也不在 DNS 转发路径上占任何位置。程序崩溃、被杀、卡死，
DNS 分流和解析**完全不受影响**。

验证结果：
- 能抓到真实域名（`rtmp01open.ipcam-cloud.com`、`api.example-video.com`、`analytics.example-cdn.com` 等）
- 解析成功率 94%
- 反查表建立 19+ 条 IP→域名映射
- 服务运行期间 DNS 一切正常

**为什么不用 eBPF**：这版内核（6.12.103）的 kprobe 不可用
（`available_filter_functions` 为空），tracepoint 里没有能拿到 DNS 报文的，
唯一可行的 XDP/TC 恰恰在转发路径上 —— 违反"不许断网"的底线。
完整分析见 `DNS_REVERSE.md`。

旧的 `dns_log` 选项（读 dnsmasq 日志）保留但默认关闭，**不要启用**。

---

## 7. 日志

```bash
# 实时
ssh -F ~/.ssh/netmon.conf r 'logread -f -e netmon'

# 只看统计（需要 loglevel=1，否则 debug 级不输出，见下）
ssh -F ~/.ssh/netmon.conf r 'logread -e netmon | grep "stat:"'

# 只看错误
ssh -F ~/.ssh/netmon.conf r 'logread -e netmon | grep "daemon\.err"'
```

### 日志级别

netmon 直接写 syslog（`LOG_DAEMON`），级别由代码决定，不写文件 ——
避免在 flash 上频繁写：

| 级别 | 内容 | 默认输出 |
|---|---|---|
| `daemon.info` | 启停、本地网段识别、规则同步 | 是 |
| `daemon.warning` | 非致命降级（嗅探起不来、homeproxy 没装） | 是 |
| `daemon.err` | 真故障（库打不开、建表失败、刷盘失败） | 是 |
| `daemon.debug` | 每秒采样明细、每分钟心跳（`stat:`） | **否** |

`stat:` 心跳默认不输出：每分钟一条、一天 1440 条，会把 logd 那点环形缓冲里
别的服务的日志挤掉。要观察时设 UCI `loglevel` 为 `1`（等价命令行 `-v`）：

```bash
uci set netmon.main.loglevel=1 && uci commit netmon && /etc/init.d/netmon restart
```

要看的时候记得改回 0，否则长期刷屏。

### busybox logread 的两个坑

1. **没有按级别过滤的选项**。`-l` 是「只取最后 N 条」，不是级别。
   `logread -l err` 会被 `atoi("err")=0` 静默解释成「不限条数」，照样输出全部 ——
   不报错，比报错更容易误判。只能用 `grep 'daemon\.err'`。
2. **没有 `-c` 清空选项**。缓冲区是环形的，靠新日志覆盖。
   排查「是不是刚重启过」要看 PID 或时间戳，不要以为缓冲区里没有旧日志。

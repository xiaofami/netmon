# netmon

OpenWrt / ImmortalWrt 上的**被动**网络监控。读 `nf_conntrack` + 旁路嗅探 udp/53，
不参与转发、不发探测包、不改防火墙 —— 装错了也不会断网。

> **本次是 1.1.0。** 上一版 `1.0.6` 修掉了 tun 代理下的流量重复计量；
> 这一版补齐 IPv6 全链路与 DNS 服务器可观测。之前的 `1.0.0` 是最早的构建，
> 有已知问题（LAN 网段写死），请用本页的最新包。版本号从 1.0.1 起按语义化递增，
> 不再在 1.0.0 上叠 `-rN`。

## 1.1.0：IPv6 全链路与 DNS 可观测（本次）

这一版回答两个新问题：**IPv6 流量到底算在谁头上**，以及**查询发给了哪台 DNS
服务器**。所有数据仍然只来自 conntrack 与 AF_PACKET 旁路，不发包、
不改 dnsmasq、不加防火墙规则。

### 新增

- **IPv6 全链路**
  - DNS 事件带完整的 16 字节客户端 / 服务器地址（v4 用 v6-mapped 存放），
    同时带 `client_fam` / `server_fam` / `server_port`，v4 / v6 两条分支都填。
  - 应答解析不再「遇到第一条 A 就停」：最多收集 8 条 A/AAAA（各带 TTL），
    并记录应答区总记录数；每条 A/AAAA 都写进域名反查表，所以 IPv6 目的地址
    也能反查到域名。
  - IPv6 噪音过滤对齐 v4：`::` / `::1`、`fe80::/10`、`ff00::/8`、
    源和目的同在本机 v6 前缀的 LAN 内互访、源是本机地址，都不再计入连接。
  - `conns` 新增 `client_ip`（LAN 侧地址）与 `dir`（0 出向 / 1 入向）。
    入向连接在采集侧已把 orig/repl 互换，`byt_o` 恒为「LAN 侧上传」、
    `byt_r` 恒为「下载」。
- **DNS 服务器可观测**
  - `dnsq` 新增 `server_ip` / `server_fam` / `server_port` / `client_fam` /
    `ttl` / `answer_cnt` / `answer_ips`，并新增 `idx_dnsq_server` 索引。
  - 报表新增 `servers` 子命令，按服务器看查询量与平均耗时。
  - `dns / devices / top / trend` 支持 `--fam 4|6|all`。
  - 新增配置 `dns_sniff_if`（默认 `br-lan`；`any` 仅用于排查抓不到查询）。
- **前端**
  - 新增「DNS 解析」视图：服务器榜（查询量 / 占比 / 均耗时）、v4 / v6 分栈对比，
    以及「服务器地址取自报文、为什么不做缓存命中率」的说明。
  - 查询日志新增「DNS 服务器」列与服务器下拉；应答 IP 下方带 TTL 小字；
    筛选条件写进 URL。
  - 导出 CSV 只导当前筛选结果，列与页面一致。

### 改进

- 所有设备归属改用 `COALESCE(client_ip, src)`：IPv6 流量、无 NAT 的 IPv6
  入向连接都能归到设备，不再被静默丢弃。
- `dashboard` 新增 `stack`（v4 / v6 的连接数、查询数、上下行、客户端数）、
  `dns_servers`；`summary` 新增 `v6_*` 与 `dns_servers_n`；
  `devices` 每行新增 `fam` / `v6_conns` / `v6_bytes_out` / `v6_bytes_in`；
  `top_cidr` / `traffic_hosts` / `top_host` / `trend` 每行新增 `fam`；
  `sysinfo` 新增 `ct_v4` / `ct_v6`。
- `dns_recent` 每行新增 `server` / `server_label` / `server_fam` / `ttl` /
  `answer_cnt` / `client_fam`。
- 告警条可点击跳到对应视图并自动应用筛选；刷新按当前视图只拉需要的数据；
  设备详情返回来源路径；空库时显示三步引导。
- DNS 服务器名称做成本地展示映射（本机 dnsmasq / 常见公共解析器 / 局域网 DNS /
  原样 IP），仅用于展示，不参与采集或转发决策。本机地址按端口区分：只有 53
  叫「本机 dnsmasq」，其它端口（例如代理自己的 DNS 口）叫「本机解析服务」。

### 修复

- **应答列语义错误**：1.1.0 之前 `answers` 列存的是 `"0"/"1"` 计数串，
  前端却把它当应答 IP 显示。新行改为存真实应答 IP（逗号分隔），旧行读时归一，
  页面显示「—」。
- **`upstream` 列被误用**：之前把首个应答 IP 写进「上游 DNS」列。
  该列从 1.1.0 起不再写入（保留列名只为老库不用重建表），报表也不再读它。
- IPv6 查询的客户端身份丢失、AAAA 应答不进入反查表、IPv6 组播 / 邻居发现
  被计成上网流量、IPv6 流量在设备归属里被丢弃，均已修复。

### 兼容性

- 所有新列走 `ensure_column()` 幂等 `ALTER TABLE ADD COLUMN`，O(1)、不重写表；
  老库用新二进制直接打开即可。
- 旧 `conns` 行 `client_ip IS NULL`、`dir` 默认 0，报表退回 `src` 口径，
  统计不变；**不做任何回填 UPDATE**。
- 旧 `dnsq` 行新字段为 NULL / 默认值：`client_fam=4`、`server_ip` 为空；
  页面按「—」显示，服务器榜只统计升级之后写入的行。
- 老配置缺 `dns_sniff_if` 时，init 脚本用 `br-lan` 兜底，不用手改。
- JSON 只增字段，不改旧字段语义；`answers` / `upstream` 的旧值由报表读时归一。

### 升级说明

- 直接安装 1.1.0 的包并重启服务即可。老库在第一次启动时自动迁移，
  **不需要手工执行 SQL，也不需要先清库**。
- 历史行的「应答 IP」列会显示「—」，因为当时根本没存真实应答地址；
  这是预期行为，不是数据损坏。从升级后的新查询开始才有真实 IP。
- `dnsq` 每行会比以前多几十字节（服务器地址、端口、协议族、TTL、应答 IP），
  库增速会略有上升；小 flash 机型可以把 `retain` 调小。
- **这一版不提供缓存命中率。** 1.1.0 开发期实现过「上游转发观测 + 时延 + TTL
  衰减」的推断，实际部署判定覆盖率不到 2%（DNS 被代理接管、上游 DoH），
  数字会误导人，已整体移除：`cache_state` / `cache_ev` 列与 `cache` JSON 都不再
  写入，页面也没有环形图和缓存列。判断解析快慢请看平均耗时与单条耗时。
- **DNS 结果分四类归因，并在采集时入库。** 每条查询写入时就按 homeproxy 的
  reject 规则、本地搜索域后缀、rcode 判成「正常 / 广告拦截 / 本地域 / 真失败」，
  存进 `dnsq.hp` 列。为什么不在报表时现算：规则集会变，事后用今天的规则去解释
  上周的查询，等于改写历史。老库的 `hp` 为 NULL，报表按 rcode + 后缀兜底，
  不需要回填。
- **`.lan` 伴生重试单独标记，查询日志里默认折叠。** 被 reject 的域名（REFUSED）
  解析失败后，客户端 resolver 会把 DHCP 下发的搜索域（本机是 `lan`）补到域名
  后面再问一次，所以 `dig.bdurl.net` 后面必然跟一条 `dig.bdurl.net.lan`。
  实测触发率约 48%，占全部查询的 4%。这类行标「域重试」，默认不显示，
  工具栏「含重试」可以打开对照；真正的内网主机名（`nas.lan`）不受影响。

## 安装包

每个架构一个 `netmon-*.apk`，外加一个所有架构通用的 `luci-app-netmon-*.apk`。

| 文件 | 适用设备 |
|---|---|
| `netmon-*-x86_64.apk` | x86 软路由、虚拟机（PVE / ESXi / Hyper-V） |
| `netmon-*-aarch64_generic.apk` | 通用 ARMv8（armsr/armv8：树莓派、多数 ARM 开发板） |
| `netmon-*-aarch64_cortex-a53.apk` | MT7981 / MT7986 系列 WiFi6 路由器（mediatek/filogic） |
| `luci-app-netmon-*.apk` | **全部架构通用**（noarch），提供 LuCI 菜单与设置页 |

```sh
apk add --allow-untrusted ./netmon-*-<你的架构>.apk ./luci-app-netmon-*.apk
```

- 包格式是 `apk-tools 3`（ADB v3），对应 **ImmortalWrt 25.x / OpenWrt 24.10+ 已启用 apk 的版本**
- 运行时依赖只有 `ip-full`、`uhttpd`、`kmod-nf-conntrack`，全部来自官方仓库
- SQLite 静态链接在二进制里，**不需要** `libsqlite3-0`
- `homeproxy` 是可选增强：没装时拦截归因自动降级，其它功能照常

装完访问 `http://<路由器 LAN IP>:8090/`；LuCI 里在「状态 → 监控面板」。
端口、监听地址、数据保留天数、采样间隔、DNS 嗅探接口都能在设置页改。

## 没覆盖到的架构

CI 只编上面三个。其它架构（ramips/mt7621、ipq40xx、bcm27xx 等）用源码自己编：

```sh
git clone https://github.com/xiaofami/netmon
cd netmon
# 从 https://downloads.immortalwrt.org/releases/ 下对应 target 的 SDK 解开
SDK_DIR=/path/to/sdk sh build/cross-build.sh build/out

# 架构名查该 target 的 profiles.json 里的 arch_packages
python3 build/build-apk.py --bin-dir build/out --arch <arch_packages> --sdk /path/to/sdk
```

整个过程不需要路由器在线，也不需要 OpenWrt 的完整构建树。

## 1.0.6：本机地址不再当作设备

开了 tun 代理（sing-box / homeproxy）之后，同一份用户流量在 conntrack 里会出现两次：

```
src=<客户端>     dst=<目标>   ← 客户端发出，被 tun 收走
src=<本机 WAN 地址> dst=<目标>   ← 代理进程从 WAN 重新发出
```

两条的字节数几乎相同。旧版只过滤「同网段互访」，没排除本机自身地址，于是：

- **域名总量翻倍**（一份流量记两遍）
- **WAN 地址被当成一台「未命名设备」**，和真设备并列在设备榜里

实测一台 x86 软路由的库里，这个问题占了 **20.6% 的记录（58437 条）**，
多算了 5.2 GB 上行、9.6 GB 下行。

1.0.6 起启动时用 `getifaddrs()` 枚举本机**所有接口地址**（LAN、WAN/PPPoE、
podman/docker 网桥、tun，含 IPv6），源地址命中就不采集。判据是「设备永远是
别的机器」——本机任何地址都不可能是设备。

顺带修好 IPv6：之前 `family != 4` 直接跳过滤，v6 的重复计量一直存在。
现在 v6 至少做本机地址排除（不做同网段判断——运营商给的是 /64，
按网段判断等于「全世界的 v6 都算内网」）。

### 老库要清一下

已经入库的重复记录不会自动消失。加了一条命令：

```sh
# 先看会删多少，不实际执行
netmon-report cleanup-self --dry-run

# 确认后执行
netmon-report cleanup-self
```

它自己识别本机地址（不用手填 IP），只删 `conns` 里源地址命中本机的行。
**注意**：`agg_hour`（按目的网段聚合）不含源地址，无法按 src 精确回滚，
趋势图里历史小时值仍会偏高，从下一个整点起才准确。

## 1.0.5：日志改走 syslog

日志之前全部走 stderr，而 procd 把它当成一个流交给 logd，没有优先级的原始行
一律标成 `daemon.err`。于是 `logread` 里连「本地网段 192.168.1.0/24」这种正常
信息都显示为错误，真出故障时反而淹没在一片 err 里分不出来。

改成直接调 `syslog(3)` 带级别写 `LOG_DAEMON`：

| 级别 | 内容 |
|---|---|
| `daemon.info` | 启停、网段识别、规则同步 |
| `daemon.warning` | 非致命降级（嗅探起不来、homeproxy 没装） |
| `daemon.err` | 真故障（库打不开、建表失败、刷盘失败） |
| `daemon.debug` | 心跳统计与采样明细，只在 `-v` 时输出 |

心跳统计默认不落盘：每分钟一条，一天 1440 条，会把 logd 那点环形缓冲里
别的服务的日志挤掉。要看的时候启动加 `-v`。

前台手跑（stderr 是终端）时日志同时打到屏幕；被 procd 接管后只走 syslog，
同一条不会记两遍。

## 1.0.1：LAN 网段动态识别

上一版（r2）里 `ignore_conn()` 用字节比较写死了两个 LAN 网段来判断「同网段互访」。
在开发那台机器上没问题，但换个网段部署就会出问题：设备之间互访、容器网络
全被当成上网流量记进库，流量榜被污染。

r3 改成启动时用 `getifaddrs()` 枚举本机所有接口的地址与掩码，运行时按掩码比对。
LAN 多网段、VLAN、docker/podman 网桥都自动覆盖，**换网段不需要改代码**。
PPPoE 的 /32 点对点地址会被跳过（它的 WAN IP 往往是 10.x，但那是真实出网流量）。

启动日志现在会打印识别到的网段，部署到新环境时一眼能看出对不对：

```
netmon: 本地网段 192.168.1.0/24（内部互访不计流量）
netmon: 本地网段 198.18.0.0/16（内部互访不计流量）
```

这条也是「跑在旁路由上」的前提：旁路由的网段几乎肯定和你原来那台不一样。

## 这些包是怎么来的

三个架构都由 GitHub Actions 在 ImmortalWrt 官方 SDK 的交叉工具链上编出来，
不是手工上传的：见 [`.github/workflows/build.yml`](.github/workflows/build.yml)。

编译走 `build/cross-build.sh`（只编二进制）→ `build/build-apk.py`（打包），
和本地从路由器取二进制的那条路用的是**同一个打包器**，所以安装脚本、
配置保留、`apk-new` 清理这些行为完全一致。

x86_64 包已实测装进一台 ImmortalWrt 25.12.2 x86 软路由：采集进程、
`netmon-report status`、CGI 与面板全部正常。

## 校验

`SHA256SUMS` 一并附上：

```sh
sha256sum -c SHA256SUMS
```

## 链接

- 在线演示（数据已脱敏）：https://tccmu.com/netmon/demo/
- 源码与文档：https://github.com/xiaofami/netmon
- 问题反馈：https://github.com/xiaofami/netmon/issues

# netmon

OpenWrt / ImmortalWrt 上的**被动**网络监控。读 `nf_conntrack` + 旁路嗅探 udp/53，
不参与转发、不发探测包、不改防火墙 —— 装错了也不会断网。

> **本次是 1.0.5。** 之前的 `1.0.0` 是最早的构建，有已知问题（LAN 网段写死），
> 请用本页的最新包。版本号从 1.0.1 起按语义化递增，不再在 1.0.0 上叠 `-rN`。

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
端口、监听地址、数据保留天数、采样间隔都能在设置页改。

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

## 1.0.5：日志改走 syslog（本次）

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
netmon: 本地网段 172.20.0.0/16（内部互访不计流量）
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

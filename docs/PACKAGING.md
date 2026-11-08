# 打包：依赖、资源与 apk 构建

> 实测日期：2026-11-07 起。数字来自真机实测，不是估算。
>
> 本文只保留有参考价值的部分：依赖从哪来、跑起来占多少资源、什么机型能带得动、
> apk 怎么构建和安装。开发过程中的调试记录不在这里。
> 安装与云端构建的最新步骤以 [README](../README.md) 为准。

## 一、实测环境

| 项 | 值 |
|---|---|
| 系统 | ImmortalWrt **25.12.2**，Linux 6.12.103，x86_64 |
| 包管理器 | **apk 3.0.5**（这一版不是 opkg，包名/命令都不同） |
| 仓库可用包 | 11 758 个（5 个 feed：base / packages / luci / routing / telephony / kmods） |
| 内存 / 存储 | 3 943 880 kB RAM，overlay 116.5 GB（用 1.8 GB） |
| 在线设备 | 53 台，24h 约 23 万次 DNS 查询 |

## 二、依赖盘点：仓库能不能全解决

### 编译期

| 依赖 | 用途 | 设备仓库 | 结论 |
|---|---|---|---|
| C 编译器 | 编译源码 | 设备上有 `gcc`，但**不能靠它做分发** | 分发必须用 OpenWrt SDK 交叉编译 |
| sqlite3 头文件 + 库 | 数据库 | `apk list \| grep -i sqlite` 只有 `libsqlite3-0`（运行时 so）和 `sqlite3-cli` | **没有设备端 dev/头文件包**。头文件只在 SDK 的 `staging_dir` 里；或继续自带 amalgamation（当前 `/opt/netmon/sqa/` 的做法） |
| libc / libgcc_s | 基础 | musl 自带 | `ldd` 显示两个二进制只依赖这两个 + ld-musl |
| `-lm` | 数学库 | musl 内置 | 无需额外包 |
| `linux/filter.h`、`linux/if_packet.h` | AF_PACKET + 经典 BPF | SDK 内核头文件 | 标准，无额外包 |

当前 `netmon` / `netmon-report` 是**静态链接 sqlite3** 的，所以运行时不依赖 `libsqlite3.so`。
改成动态链接可以省下约 1 MB 二进制，但要加 `+libsqlite3-0` 依赖，两者都可行。

### 运行时

| 依赖 | 用途 | apk 包名 | 仓库 |
|---|---|---|---|
| IPv6 → MAC 反查 | `ip -6 neigh show` | **`ip-full`**（`ip-tiny` 可能不含 neigh） | ✅ 已装 |
| 页面 + CGI | 独立 uhttpd 实例 | `uhttpd` | ✅ 已装；25.12 的 uhttpd **主程序内置 CGI**，仓库已无 `uhttpd-mod-cgi` 包，别写进依赖 |
| UCI 配置 | 读 `/etc/config/dhcp`、`/etc/config/homeproxy` | 代码自己解析文本，不 fork `uci` | ✅ 无需二进制 |
| homeproxy 规则反编译 | `sing-box rule-set decompile` | `sing-box` | ✅ 仓库有（1.12.25）；**可选**，检测不到时自动降级 |
| 内核 AF_PACKET / SO_ATTACH_FILTER | DNS 旁路嗅探 | `CONFIG_PACKET` + 经典 BPF | ✅ `/proc/net/packet` 存在 |
| conntrack 采样 | `/proc/net/nf_conntrack` | `kmod-nf-conntrack*` | ✅ 已装 |
| `python3` | 仅 `build/` 里的本地验证工具 | — | **运行时不需要**，别写进依赖 |
| `sqlite3` CLI | 无（C 直接调 API） | `sqlite3-cli` | **不需要** |

**结论：运行时依赖 100% 能由官方仓库解决**，且大部分是标准 OpenWrt 已装组件。
前端是单文件原生 JS，**零 CDN、零 npm、零构建步骤**，也没有外部依赖。
唯一的例外是**编译期 sqlite3 头文件**：设备仓库没有 dev 包，必须走 SDK 或把 amalgamation 放进包源码。

## 三、资源实测

| 指标 | 实测值 | 说明 |
|---|---|---|
| `netmon` RSS | **83.6 MB** | 单进程、单线程 |
| VmSize / VmData | 249 MB / 210 MB | 虚拟地址空间，不是实占 |
| CPU | **单核 0.08%** | 3 秒采样：进程 1 tick / 全机 1200 tick |
| 打开文件描述符 | 7 | 一个 AF_PACKET socket + db + 日志 |
| 数据库体积 | **36 MB** | 保留期 30 天配置下运行约 1.5 天 |
| 增长速率 | conns 15.2 万行/天 + dnsq 17.8 万行/天 ≈ **33 万行/天，约 24 MB/天** | 30 天默认会到 **≈ 700 MB** |
| `dashboard` 单次 | **3.2 秒**（x86_64） | 24h 窗口内对 27 万条 dnsq 做多次全窗口 GROUP BY |
| `detail` 单次 | 340 ms | 走索引，快得多 |
| 二进制 | 1.1 MB + 1.1 MB | 静态链接 sqlite3 |
| 前端 | 124 KB HTML | 单文件 |

**RSS 为什么这么大**：`db.c` 默认 `PRAGMA cache_size=-65536`（64 MB SQLite 页缓存）+
连接采样的开放寻址哈希表。这个值可通过 `db_open()` 参数调小，降到 8 MB 时
RSS 大约能压到 25–30 MB，是适配小内存设备最有效的一刀。

**真正的性能瓶颈是 dashboard 的 3.2 秒**，不是采集。10 秒自动刷新在 x86 上
相当于常驻约 1/3 个核；低端 MIPS/ARM 会更慢。分发前建议三选一：
1. 自动刷新放宽到 30–60 秒（前端一行配置）；
2. 给 dashboard 加 10–30 秒结果缓存（CGI 层或 netmon 进程内）；
3. 用已有的 `agg_hour` / `agg_day` 预聚合表替代部分全窗口扫描。

## 四、最低需求与主流机型

| 资源 | 最低可用 | 推荐 | 说明 |
|---|---|---|---|
| RAM | **128 MB** | ≥ 256 MB | 需把 `cache_size` 降到 8 MB；系统 + LuCI 本身约 60–80 MB |
| 可写存储 | **≥ 256 MB**（保留 3–7 天） | ≥ 1 GB / USB / SSD | 默认 30 天约 700 MB，小 Flash 一定爆 |
| CPU | 任意支持 AF_PACKET 的 MIPS/ARM/x86 | 双核 ≥ 800 MHz | 采集只要 0.08%，瓶颈是 SQLite 查询 |
| 内核 | `CONFIG_PACKET`、经典 BPF (`SO_ATTACH_FILTER`)、`CONFIG_NF_CONNTRACK` + procfs | 同上 | 标准 OpenWrt 内核都满足 |
| 权限 | root（`CAP_NET_RAW`） | root | procd 默认以 root 启动 |

| 机型 | RAM / Flash | 结论 | 必要条件 |
|---|---|---|---|
| MT7628 / AR9331 老路由 | 64 MB / 8–16 MB | ❌ 不支持 | Flash 与 RAM 都不够 |
| MT7621（新路由 3、K2P 等） | 256 MB / 16–32 MB | ⚠️ 勉强 | 必须 USB 存储 + 保留期 ≤3 天 + cache 8 MB + 刷新 60 秒 |
| MT7981（AX3000 类） | 256 MB / 128 MB | ⚠️ 可用 | 保留期 ≤7 天或外置存储，dashboard 刷新放慢 |
| MT7986（AX6000 类） | 512 MB / 256 MB | ✅ | 建议保留期 ≤14 天或 USB |
| x86 软路由 | ≥2 GB / SSD | ✅ 无压力 | 当前环境 |
| 树莓派 / NanoPi | ≥1 GB / SD | ✅ | 注意 WAL 频繁写对 SD 卡寿命的影响 |

**一句话：限制是存储，不是 CPU 或内存。** 分发版必须把默认保留期从 30 天
降到 3–7 天，并允许把 `db_path` 配到外置存储，否则绝大多数家用路由器装完几天就写满。

## 五、打包成 LuCI 应用：可行性

### 现状与 LuCI 体系的差距

1. 当前是**独立 uhttpd 实例**：`/etc/config/uhttpd` 里的 `uhttpd.netmon` section，
   监听 `192.168.1.1:8090`，`home=/www/netmon`，`cgi_prefix=/cgi-bin`。
2. 数据接口是 shell CGI → `netmon-report` 输出 JSON；LuCI 原生做法是
   `rpcd` + ubus 方法 + ACL。
3. 前端是独立 SPA，由静态文件服务提供；LuCI 通常用 `luci-base` 的 dispatcher
   + 模板 / `luci.http`。
4. 当前在设备上直接 `gcc` 编译；分发必须走 OpenWrt SDK 交叉编译。

### 推荐路径：拆两个包，保留现有前后端

- **`netmon`**（架构相关）
  - `DEPENDS:=+curl +ip-full +uhttpd +kmod-nf-conntrack`（若改为动态链接再加 `+libsqlite3-0`）
  - `PKG_BUILD_DEPENDS:=libsqlite3`（SDK 提供头文件）
  - 装：`/usr/bin/netmon`、`/usr/bin/netmon-report`、`/etc/init.d/netmon`、
    `/etc/config/netmon`、`/www/netmon/{index.html,cgi-bin/data}`
  - 静态链接时可把 sqlite amalgamation 放进包源码，省掉 dev 依赖（+约 250 KB 源码）
- **`luci-app-netmon`**（`PKGARCH:=all`）
  - `LUCI_DEPENDS:=+luci-base +netmon`
  - 菜单项：`luasrc/controller/netmon.lua` 里
    `entry({"admin","network","netmon"}, template("netmon/index"), _("网络监控"), 60)`
  - 最小做法：模板里用 iframe 嵌入现有 8090 页面，或把 SPA 拷到
    `/luci-static/resources/view/netmon/` 由 LuCI 直接渲染。
  - **不用改前端、不用改后端**，只加一层菜单与打包元数据。

### 障碍清单

| 障碍 | 处理 |
|---|---|
| 交叉编译 | 写 package Makefile 的 `Build/Compile`，链接 SDK staging 的 libsqlite3 |
| sqlite 头文件设备仓库没有 | 用 SDK；或把 amalgamation 打进包源码 |
| `/opt/netmon/data/` 在小 Flash 上不存在 | 默认改到 `/etc/netmon/` 或 `/tmp`，UCI 可配外置路径 |
| 默认 30 天保留 ≈700 MB | 分发版默认 7 天，UCI 可调 |
| 独立 uhttpd 实例与 LuCI 80 端口并存 | 当前已验证可行；也可改挂主实例的 `uhttpd` section |
| homeproxy 规则同步 | 声明为可选依赖；检测不到 `sing-box` 时自动降级（代码已支持） |
| apk 而非 opkg | 25.12 用 apk，但包的 `DEPENDS`/`LUCI_DEPENDS` 写法由构建系统翻译，写法不变；**别误加不存在的 `sqlite3-dev` / `uhttpd-mod-cgi`** |
| CGI 权限 | `files/` 里带 755 权限，shell 用 busybox `ash` 兼容语法 |
| AF_PACKET 需要 root | procd 服务默认 root ✅ |

### 工作量与结论

| 方案 | 工作量 | 说明 |
|---|---|---|
| 基础包 + LuCI 菜单（iframe） | 1–2 天 | 含 SDK 编译、至少两台不同架构实测 |
| 改成原生 rpcd/ubus + LuCI 视图 | 3–5 天 | 要重写接口层，前端 fetch 改路径 |
| 低资源机型适配（缓存 / 保留期 / 查询优化） | +1–2 天 | 主要是 dashboard 的 3.2 秒 |

---

## 六、产物与构建

### 产物

三个架构各一个 `netmon-*.apk`，外加一个全架构通用的 LuCI 包：

| 文件 | 架构 | 运行时依赖（全部来自官方 apk 仓库） |
|---|---|---|
| `netmon-<ver>-x86_64.apk` | x86_64 | `libc`, `ip-full`, `uhttpd`, `kmod-nf-conntrack` |
| `netmon-<ver>-aarch64_generic.apk` | aarch64_generic | 同上 |
| `netmon-<ver>-aarch64_cortex-a53.apk` | aarch64_cortex-a53 | 同上 |
| `luci-app-netmon-<ver>-noarch.apk` | noarch | `luci-base`, `luci-compat`, `netmon` |

SQLite 以 amalgamation **静态链接**进二进制（`ldd` 只有 musl libc + libgcc），
所以不需要 `libsqlite3-0`。homeproxy / sing-box 是可选增强，不写进 depends。

### 构建

分两步，都在 `.github/workflows/build.yml` 里跑：

1. `build/cross-build.sh` —— 用 SDK 的交叉工具链编出 `netmon` 与 `netmon-report`，
   SQLite amalgamation 一并交叉编译后静态链接
2. `build/build-apk.py` —— 组装文件树、写入 `post-install` / `pre-deinstall`、
   用 SDK 自带的 `staging_dir/host/bin/apk mkpkg` 生成 **ADB v3** 包
   （文件头 `ADBd`，apk-tools 3 的原生格式，不是 gzip-tar，手工拼不出来）

拆成两步的好处：打包器与本地从路由器取二进制时用的是**同一份**，安装/卸载脚本
不会因为构建路径不同而跑偏。手编步骤见 [README](../README.md)。

**没走 `make package/xxx/compile`** 的原因：那需要把仓库塞进 SDK 的 `package/`
并按 buildroot 的包框架走一遍（feeds 更新、defconfig、依赖解析），慢且容易因上游
变动而红。`apk mkpkg` 是原生工具，不依赖包扫描。`package/` 下两个 Makefile 仍保留，
供将来完整 SDK make 使用。

### 安装 / 卸载

```sh
apk add --allow-untrusted ./netmon-<ver>-<arch>.apk ./luci-app-netmon-<ver>-noarch.apk
apk del netmon luci-app-netmon
```

未签名的本地包需要 `--allow-untrusted`。安装后 `post-install` 自动创建 uhttpd 实例、
启用并启动采集。LuCI 菜单在「状态 → 监控面板」。

### 打包时容易踩的坑

这几条都是实际踩过并修掉的，改打包逻辑时注意别退回去：

1. **`mkpkg` 的数组字段**：`-I depends:X` 重复传只会保留**最后一个**，依赖被悄悄
   丢光。必须一次给一个空格分隔串。`build-apk.py` 现在断言依赖条数，防止复发。
2. **`apk del` 会 purge 配置**：`/etc/config/netmon` 被删，用户改过的端口/保留期丢失。
   `pre-deinstall` 把配置存到数据目录，`post-install` 恢复。
3. **procd respawn**：`stop` 后进程又被拉起，卸载后残留孤儿进程。
   要先 `disable` 再 `stop`，并 `killall` 兜底。
4. **uhttpd reload 是异步的**：改端口后立刻访问可能撞 502。
   `netmon-apply` 用轮询等新端口就绪后才返回。
5. **`.apk-new` 残件**：apk 检测到配置被改动会把包内默认配置另存为 `.apk-new`，
   且是在 `post-install` **跑完之后**才落盘，脚本里立刻删会扑空。
   实际有效的位置是 `/etc/init.d/netmon`，每次启动扫一次。


### 已知边界

- 预编译包只覆盖 x86_64 / aarch64_generic / aarch64_cortex-a53 三个架构；
  其它架构（ramips、ipq40xx 等）用对应 target 的 SDK 按 README 的步骤自己编。
- 包未签名，需 `--allow-untrusted`。要做成能 `apk add` 直接装的仓库，
  得自己生成签名密钥并配 apk 索引。
- LuCI 设置页语法（`luac -p`）与路由存在性（301 跳登录页）已验证，
  但**未登录实际点验**（需要 LuCI 登录凭据）。
- 独立端口无鉴权，默认 `listen_addr=lan` 只监听 LAN；改成 `0.0.0.0` 需自行加防火墙规则。
- 修改 `db_path` 不自动迁移历史数据（设置页已写明）。

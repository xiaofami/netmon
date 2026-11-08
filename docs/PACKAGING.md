# 依赖、资源与 LuCI 打包可行性评估

> 实测日期：2026-11-07。所有数字来自当前运行的路由器，不是估算。

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
| HTTP 探测 / 代理可达性探测 | `probe.c`、`reach.c` 调 `curl` | `curl` | ✅ 已装，仓库有 |
| ICMP 探测 | `ping` / `ping6` | busybox 自带（或 `iputils-ping`） | ✅ |
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

**结论：方便，但不是零成本。** 底子很好——运行时依赖全在官方仓库、前端零依赖、
采集 CPU 几乎为零、纯 C 无解释器。主要成本在三处：交叉编译适配、
把默认存储参数降到家用路由器能承受的范围、以及把独立 uhttpd/CGI 接进 LuCI 菜单。
建议先发「基础包 + iframe 菜单」这一版，验证过再逐步原生化。

## 六、排版审查（同日）

用真实 Chrome 无头渲染 + 注入脚本测 `scrollWidth/clientWidth` 复核，修掉的问题：

- 总览图表容器带内联 `grid-template-columns:1fr 1fr`，**优先级高于媒体查询**，
  窄屏两个图挤成各 110px；改由 `.grid2` 的 CSS 统一管理。
- `.app` 在 ≤900px 用 `grid-template-columns:1fr`，`1fr` 的最小尺寸是 min-content，
  侧栏横向导航会撑宽整列导致右侧被裁；改成 `minmax(0,1fr)` 并给 side/main 加 `min-width:0`。
- 部分行带内联 grid 定义，卡片模式必须用 `!important` 才能覆盖。
- KPI 副标题与右下角迷你趋势线重叠 → `.kpi-foot.with-spark{padding-right:82px}`，
  副标题 `min-height:30px` 允许两行不再被裁。
- KPI 长数值（`2.24G / 148.81M`）折行 → 按视觉宽度（中文算两格）自动切
  `compact` / `tiny` 字号并 `white-space:nowrap`。
- 隐私观察两个半宽面板改全宽，IPv6 地址列加宽，39 字符地址从挤成三行变一行完整显示。
- 代理建议详情不再摆四个空表；说明文字允许换行，不再被省略号截成半句。

验证：iframe 精确模拟 **390 CSS px**，`scrollWidth=381`、无横向溢出、0 个被裁元素；
桌面 1600px 各视图静态检查与 93 项渲染断言全过。

> 注：Chrome 无头截图的最小布局宽度是 500px，直接 `--window-size=390` 会把
> 500px 的布局裁成 390px 图片，看起来像「右侧显示不全」，其实是工具假象。
> 验证真机宽度要用 iframe 或 DevTools 的 device metrics override。


---

## 2026-11-07 更新：包骨架已落地

上文的评估结论不变，但**不再只是评估**：`package/` 下已有可直接放进 SDK 的
两个包骨架，`package/netmon/files/` 里已组装好全部源码、init.d、UCI 配置、
页面、CGI 与 uhttpd 实例脚本。详见 `package/README.md`。

关键调整：

- **homeproxy / sing-box 不写进 `DEPENDS`**，作为可选增强。
  实测把 `/etc/config/homeproxy` 临时移走后：`hp.available=0`、
  `block_stats.blocked=0`、其余字段齐全、JSON 合法，界面显示降级说明；
  配置恢复后 30 秒内自动重新启用。
- `DEPENDS` 只列 `+libsqlite3-0 +curl +ip-full +uhttpd +kmod-nf-conntrack`。
- 采集进程已不再做主动探测，包的运行时出站流量为零（除了规则反编译时的
  本地 `sing-box` 调用）。
- 仍待完成：用目标 SDK 交叉编译实测（本机是 x86，无法验证其它架构），
  以及把 iframe 方案升级成 rpcd/ubus 以统一 LuCI 鉴权。


---

## 2026-11-08 评估：方便打包成 LuCI 应用吗？

**结论：方便，而且本轮已经把最关键的「可配置」部分做完了。**
用户点名的三件事——日志存储时间、位置、页面端口——现在都是 UCI 项，
在 LuCI 设置页改完保存即生效，体验与 `luci-app-adguardhome` 的配置页一致。

### 已经具备

| 能力 | 状态 |
|---|---|
| `netmon` + `luci-app-netmon` 两包骨架、完整 `files/` | ✅ 已齐 |
| 运行时依赖 100% 来自官方 apk 仓库 | ✅ 已核 |
| homeproxy 作为可选依赖、不写进 `DEPENDS` | ✅ 已降级并实测 |
| CBI 设置页（端口 / 监听地址 / 数据库位置 / 保留天数 / 采样间隔 / 启用 / 调试日志） | ✅ 本轮新增 |
| 配置保存后自动生效（`netmon-apply`） | ✅ 本轮新增，线上实测通过 |
| 独立 uhttpd 实例端口可改 | ✅ 本轮新增 |
| 升级保留用户配置（`conffiles`） | ✅ 本轮新增 |
| CBI 依赖 `luci-compat` 可解 | ✅ 官方仓库有、本机已装、运行时可 require |
| 修掉 `init.d` 里 `PROG=/opt/netmon/src/netmon` 的打包隐患 | ✅ 改为 `/usr/bin/netmon` |

### 与 luci-app-adguardhome 的区别（架构决定，不是缺口）

AdGuard Home 自身是可配置的 DNS 过滤器，LuCI 包自然能管拦截规则、上游、
客户端策略；netmon 是**只读旁路监控**，不进数据面，所以设置页只管
「采集参数 + 面板端口」，不会有拦截开关。要想有那些能力得另起一个过滤类应用。

### 剩余工作与成本

| 项 | 估计 | 说明 |
|---|---|---|
| 目标 SDK 交叉编译 + 多架构验证 | 约 1 天 | 本机 x86 无法代验 MT7621/7981/7986 等 |
| 设置页装包后浏览器点验 | 半天 | 语法与运行时框架已验证，差真机点一遍 |
| 默认参数按机型收敛 | 半天 | 30 天保留约 700 MB，小 Flash 机型应默认 3–7 天 + 外置存储 |
| 统一 LuCI 鉴权（rpcd/ubus 化） | 3–5 天 | 可选；不做则用独立端口 + 限制监听地址 |

**打包前必须处理的两个风险**：
1. 独立端口无鉴权。默认 `listen_addr=lan` 会自动解析成 LAN 地址，
   不会监听 WAN；用户若显式改成 `0.0.0.0`，必须自行加防火墙规则
   （设置页与 `OPERATIONS.md` 都已明确警告）。
2. 修改 `db_path` 后历史数据留在旧位置、不会自动迁移，设置页已写明，
   避免用户以为数据丢了。

### 本轮线上验证

`/usr/bin/netmon-apply` 实际执行成功；uhttpd 监听保持 `192.168.1.1:8090`；
采集进程以 `exe=/usr/bin/netmon`、`-d /opt/netmon/data/netmon.db -i 5000 -r 30` 启动；
页面、dashboard、client 全 200。`luac -p` 与 `sh -n` 全部通过。


---

## 2026-11-08 实测：x86_64 apk 已产出并安装验证

用户要求「动手打包成 apk，只考虑 x86 平台，依赖尽量来自软件仓库」。
**已完成**：两个包在 `netmon/dist/`，路由器上实测安装、卸载、重装、改端口全部通过。

### 产物

| 文件 | 架构 | 依赖（全部来自官方 apk 仓库） |
|---|---|---|
| `netmon-1.0.5-x86_64.apk` | x86_64 | `libc`, `ip-full`, `uhttpd`, `kmod-nf-conntrack` |
| `luci-app-netmon-1.0.5-noarch.apk` | noarch | `luci-base`, `luci-compat`, `netmon` |

sqlite3 以 amalgamation **静态链接**进二进制（`ldd` 只有 musl libc + libgcc），
所以不再需要 `libsqlite3-0`；主动探测停用后也不再需要 `curl`。
homeproxy / sing-box 仍是可选增强，不写进 depends。

### 构建方式（`build/build-apk.py`，可复现）

1. **二进制**：在目标机用 `x86_64-openwrt-linux-musl gcc` 编译当前源码
   （x86 单一平台，ABI 与本机完全一致，`-O2 -Wall -Wextra` 零警告）。
2. **打包**：用 ImmortalWrt x86_64 SDK 自带的 `staging_dir/host/bin/apk mkpkg`
   生成 **ADB v3**（文件头 `ADBd`）包——这是 apk-tools 3.0.5 的原生格式，
   不是老式 gzip-tar，手工拼不出来。
3. 组装文件树、写入 `post-install` / `pre-deinstall` 脚本、生成包、
   用 `apk adbdump` 校验依赖数量与脚本。

SDK 下载：`releases/25.12.2/targets/x86/64/immortalwrt-sdk-25.12.2-x86-64_gcc-14.3.0_musl.Linux-x86_64.tar.zst`
（sha256 `4d4631f134c2f27215a91aa7ef5bf9a01ea96c2450d3b8a5c38048c32462ac60`）。
本项目只保留了它的 `staging_dir/host/`（约 900 MB，供 mkpkg 用），
`build_dir/`（2.3 GB）和原始压缩包已删。要重建 mkpkg 环境需重新下载解压。

**为什么没走 `make package/xxx/compile`**：SDK 解压被中断过一次
（归档属主 uid 1000 + 沙箱输出限制），工具链 `sys-include` 损坏、
`prepare-tmpinfo` 失败；而 `apk mkpkg` 是原生工具，不依赖 buildroot 的包扫描，
更直接也更可靠。`package/` 下两个 Makefile 仍保留，供将来完整 SDK make 使用。

### 安装 / 卸载

```sh
apk add --allow-untrusted netmon-1.0.5-x86_64.apk luci-app-netmon-1.0.5-noarch.apk
apk del netmon luci-app-netmon
```

未签名的本地包需要 `--allow-untrusted`。安装后 `post-install` 会自动创建
uhttpd 实例、启用并启动采集服务。LuCI 菜单：网络 → Network Monitor
（监控面板 / 设置）。

### 实测结果（2026-11-08）

- `apk add --simulate` 依赖解析通过；实际安装输出 `Installing` + 执行 `post-install`。
- `apk info -L netmon` 文件归属正确；8 个文件 + LuCI 3 个文件全部落位。
- 服务 `running`，uhttpd 监听 `192.168.1.1:8090`，页面 / dashboard / client 全 200。
- **改端口实测**：`http_port` 8090→8091 后 `uhttpd.netmon.listen_http` 同步变化，
  8091 页面与 CGI 都 200；恢复 8090 后全部 200。
- **卸载 / 重装闭环**：卸载后进程停干净、配置备份生成；重装后配置逐字节恢复。

### 打包过程中踩到并修掉的坑

1. **`mkpkg` 数组字段**：`-I depends:X` 重复传只会保留最后一个，依赖被悄悄丢光。
   必须一次给一个空格分隔串：`-I "depends:libc ip-full uhttpd kmod-nf-conntrack"`。
   构建脚本现在会断言依赖条数，防止复发。
2. **`apk del` 会 purge 配置**：`/etc/config/netmon` 被删，用户改的端口/保留期丢失。
   `pre-deinstall` 把配置存到 `/opt/netmon/data/.netmon.conf.bak`，
   `post-install` 恢复并删除备份。
3. **procd respawn**：`stop` 后进程又被拉起，卸载后残留孤儿进程。
   改成先 `disable` 再 `stop`，并 `killall netmon` 兜底。
4. **uhttpd reload 异步**：改端口后立刻访问可能撞 502。
   `netmon-apply` 现在用 `wget` 轮询新端口，就绪后才返回。
5. **`.apk-new` 残件**：apk 检测到配置被改动会把包内默认配置另存，
   `post-install` 主动清理。
6. **测试假象**：宿主机有 HTTP 代理（`127.0.0.1:39071`），访问未放行的端口会拿到
   代理返回的 502，看起来像面板故障；且 `trap restore` 在 ssh 会话退出时就恢复了端口，
   宿主机随后 curl 新端口必然失败。端口验证必须在同一会话内、用 `--noproxy '*'` 做。

### 已知边界

- **仅 x86_64**，包未签名，需 `--allow-untrusted`；其它架构要用对应 SDK 重编二进制。
- LuCI 设置页语法（`luac -p`）与路由存在性（301 跳登录页）已验证，
  但**未登录实际点验**（需要 LuCI 登录凭据）。
- 独立端口无鉴权，默认 `listen_addr=lan` 只监听 LAN；改成 `0.0.0.0` 需自行加防火墙规则。
- 修改 `db_path` 不自动迁移历史数据（设置页已写明）。


---

## 2026-11-08 修复：LuCI 内嵌面板「加载不出来」

### 现象
LuCI 里「监控面板」空白；「在新窗口打开面板」指向
`https://router.example.lan:8090/`，同样打不开；只有手动访问
`http://192.168.1.1:8090/` 正常。

### 三个叠加的根因

| # | 原因 | 说明 |
|---|---|---|
| 1 | **URL 拼错**：view 用 `HTTP_HOST` + 端口拼地址 | 本环境 `HTTP_HOST=router.example.lan`，于是得到 `https://router.example.lan:8090/`。Caddy 只把 443 反代到 uhttpd 的 `127.0.0.1:8080`，**没有监听 8090**；独立 uhttpd 实例只绑 LAN IP，外部域名连不上 |
| 2 | **混合内容被拦** | 即便改成 `http://LAN_IP:8090`，LuCI 走 HTTPS 时浏览器会阻止 iframe 加载 http 子页面 |
| 3 | **`X-Frame-Options: DENY`**（最隐蔽） | Caddy 的 `security_headers` 给全站下发 `DENY`，**同源也禁止**。URL 全对，iframe 依然白屏 |

### 修法

**面板改走 LuCI 主实例的同源路径**，与端口、域名、协议彻底解耦：

```
页面：/netmon/                  uhttpd 主实例 home=/www 直接提供静态文件
API ：/cgi-bin/netmon-data       同一 cgi_prefix=/cgi-bin 下执行
```

- `view` 的 iframe 改为 `src="/netmon/"`；「新标签打开」同源；
  另留一个「直连独立端口」链接，按 UCI 的 `listen_addr`（`lan` 会解析成 LAN IP）生成。
- 前端 API 基址抽成 `apiBase()`：页面在 `/netmon/` 下用 `/cgi-bin/netmon-data`，
  其余情况用 `/cgi-bin/data`，**两种部署方式共用同一份前端**。
- apk 同时安装两份 CGI：`/www/netmon/cgi-bin/data`（独立端口）与
  `/www/cgi-bin/netmon-data`（LuCI 主实例）。`netmon-apply` 也会补齐后者，
  手工部署同样生效。
- Caddy 侧只对面板路径放开 frame 限制，其余路径与其它站点保持 `DENY`：

```caddyfile
@netmon_panel path /netmon /netmon/* /cgi-bin/netmon-data
header @netmon_panel {
    -X-Frame-Options
    Content-Security-Policy "frame-ancestors 'self'"
}
```

  该改动已同步到项目的 `caddy/sites/router.example.lan.conf`。

### 验证

| 项 | 结果 |
|---|---|
| `https://router.example.lan/netmon/` | 200，145 KB |
| `https://router.example.lan/cgi-bin/netmon-data?since=86400` | 200，74 KB，JSON 合法（52 台设备） |
| `/netmon/` 响应头 | 无 `X-Frame-Options`，`Content-Security-Policy: frame-ancestors 'self'` |
| LuCI 其它路径响应头 | 仍为 `X-Frame-Options: DENY`（安全策略未被削弱） |
| 真实 Chrome 同源 iframe | `hasKpi=true`，KPI 渲染出真实数据（DNS 查询 219,183 / 活跃客户端 24 / 拦截 81,782），`api=/cgi-bin/netmon-data` |
| 直连 `http://192.168.1.1:8090/` | 200，未受影响 |

### 附带改进

- **首屏骨架**：dashboard 冷启动约 4–5 秒，KPI 区原本空白、在 iframe 里像加载失败。
  现在先显示「正在读取监控数据…」，数据到达后被真实内容替换。
- **消除打包副本漂移**（真 bug）：`build-apk.py` 原本从 `package/netmon/files/`
  的**副本**取 `index.html`，而开发改的是 `www/index.html`，导致包内前端落后两个版本
  （装完线上版本戳从 `d` 退回 `b`）。已改为直接取 `www/` 与 `src/` 单一来源，
  并在打包时打印前端版本戳便于核对。
- 新增回归断言：`apiBase()` 在 `/` 与 `/netmon/` 两种路径下分别返回
  `/cgi-bin/data` 与 `/cgi-bin/netmon-data`，换回根路径能恢复。

### 安全说明

`/netmon/` 与 `/cgi-bin/netmon-data` 走 Caddy 反代，受 `internal_access_control`
的 **LAN 网段白名单**保护（`192.168.1.0/24` 等），且只放开 frame 限制、
不改访问控制。但它**不经过 LuCI 登录鉴权**——同一 LAN 内知道地址即可查看，
与之前直连 8090 的安全等级一致。若需要统一登录，需改造成 rpcd/ubus 接口。


---

## 2026-11-08 修复：LuCI 内嵌面板「加载不出来」

### 现象
LuCI 里「监控面板」空白；「在新窗口打开面板」指向
`https://router.example.lan:8090/`，同样打不开；只有手动访问
`http://192.168.1.1:8090/` 正常。

### 三个叠加的根因

| # | 原因 | 说明 |
|---|---|---|
| 1 | **URL 拼错**：view 用 `HTTP_HOST` + 端口拼地址 | 本环境 `HTTP_HOST=router.example.lan`，于是得到 `https://router.example.lan:8090/`。Caddy 只把 443 反代到 uhttpd 的 `127.0.0.1:8080`，**没有监听 8090**；独立 uhttpd 实例只绑 LAN IP，外部域名连不上 |
| 2 | **混合内容被拦** | 即便改成 `http://LAN_IP:8090`，LuCI 走 HTTPS 时浏览器会阻止 iframe 加载 http 子页面 |
| 3 | **`X-Frame-Options: DENY`**（最隐蔽） | Caddy 的 `security_headers` 给全站下发 `DENY`，**同源也禁止**。URL 全对，iframe 依然白屏 |

### 修法

**面板改走 LuCI 主实例的同源路径**，与端口、域名、协议彻底解耦：

```
页面：/netmon/                  uhttpd 主实例 home=/www 直接提供静态文件
API ：/cgi-bin/netmon-data       同一 cgi_prefix=/cgi-bin 下执行
```

- `view` 的 iframe 改为 `src="/netmon/"`；「新标签打开」同源；
  另留一个「直连独立端口」链接，按 UCI 的 `listen_addr`（`lan` 会解析成 LAN IP）生成。
- 前端 API 基址抽成 `apiBase()`：页面在 `/netmon/` 下用 `/cgi-bin/netmon-data`，
  其余情况用 `/cgi-bin/data`，**两种部署方式共用同一份前端**。
- apk 同时安装两份 CGI：`/www/netmon/cgi-bin/data`（独立端口）与
  `/www/cgi-bin/netmon-data`（LuCI 主实例）。`netmon-apply` 也会补齐后者，
  手工部署同样生效。
- Caddy 侧只对面板路径放开 frame 限制，其余路径与其它站点保持 `DENY`：

```caddyfile
@netmon_panel path /netmon /netmon/* /cgi-bin/netmon-data
header @netmon_panel {
    -X-Frame-Options
    Content-Security-Policy "frame-ancestors 'self'"
}
```

  该改动已同步到项目的 `caddy/sites/router.example.lan.conf`。

### 验证

| 项 | 结果 |
|---|---|
| `https://router.example.lan/netmon/` | 200，145 KB |
| `https://router.example.lan/cgi-bin/netmon-data?since=86400` | 200，74 KB，JSON 合法（52 台设备） |
| `/netmon/` 响应头 | 无 `X-Frame-Options`，`Content-Security-Policy: frame-ancestors 'self'` |
| LuCI 其它路径响应头 | 仍为 `X-Frame-Options: DENY`（安全策略未被削弱） |
| 真实 Chrome 同源 iframe | `hasKpi=true`，KPI 渲染出真实数据（DNS 查询 219,183 / 活跃客户端 24 / 拦截 81,782），`api=/cgi-bin/netmon-data` |
| 直连 `http://192.168.1.1:8090/` | 200，未受影响 |

### 附带改进

- **首屏骨架**：dashboard 冷启动约 4–5 秒，KPI 区原本空白、在 iframe 里像加载失败。
  现在先显示「正在读取监控数据…」，数据到达后被真实内容替换。
- **消除打包副本漂移**（真 bug）：`build-apk.py` 原本从 `package/netmon/files/`
  的**副本**取 `index.html`，而开发改的是 `www/index.html`，导致包内前端落后两个版本
  （装完线上版本戳从 `d` 退回 `b`）。已改为直接取 `www/` 与 `src/` 单一来源，
  并在打包时打印前端版本戳便于核对。
- 新增回归断言：`apiBase()` 在 `/` 与 `/netmon/` 两种路径下分别返回
  `/cgi-bin/data` 与 `/cgi-bin/netmon-data`，换回根路径能恢复。

### 安全说明

`/netmon/` 与 `/cgi-bin/netmon-data` 走 Caddy 反代，受 `internal_access_control`
的 **LAN 网段白名单**保护（`192.168.1.0/24` 等），且只放开 frame 限制、
不改访问控制。但它**不经过 LuCI 登录鉴权**——同一 LAN 内知道地址即可查看，
与之前直连 8090 的安全等级一致。若需要统一登录，需改造成 rpcd/ubus 接口。


### 菜单位置：状态 → 监控面板

按用户要求把插件从「网络」移到 **「状态」** 下，名称为 **「监控面板」**：

```
状态 → 监控面板 → [面板 | 设置]
```

- 父节点用 `firstchild()`，点侧栏「监控面板」直接进面板页，右侧 tab 切到「设置」。
- order 60，排在状态菜单靠后，不打乱概览 / 路由等原有序位。
- 新版 LuCI（26.x）用 `/usr/share/luci/menu.d/*.json` 定义一级菜单，
  本包仍走 Lua controller（`luci-compat` 提供兼容层），
  `admin/status` 由 `luci-mod-status` 提供，挂载在其下无需额外 JSON。

改菜单后必须清 LuCI 的菜单缓存，否则侧栏位置不会变：

```sh
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache*
```

**验证方法**（不依赖登录，`build/luci-menu-probe.lua`）：
把 `entry/firstchild/template/cbi` 打桩后执行 controller 的 `index()`，
打印实际注册的路径与标题。安装后实测输出：

```
path=admin/status/netmon          title=监控面板 order=60
path=admin/status/netmon/panel    title=面板     order=10
path=admin/status/netmon/settings title=设置     order=20
```

> 注意：未登录时 LuCI 对所有 admin 路径都返回 403，
> 不能靠 HTTP 状态码判断路由是否存在，必须用上面的打桩法或登录后目视。


---

## 2026-11-08 修复二：`admin/status/netmon` 报 Runtime error

### 真实错误堆栈

```
/usr/lib/lua/luci/view/netmon/index.htm:10: attempt to call method 'gsub' (a nil value)
stack traceback:
    /usr/lib/lua/luci/view/netmon/index.htm:10: in main chunk
    [C]: in function 'xpcall'
    /usr/share/ucode/luci/template.uc:23: in function <anonymous>
```

第 10 行是 `addr = addr:gsub("/.*$", "")`。

### 根因：新版 LuCI 的 `uci:get()` 对标量也返回 table

实测：

```lua
local c = require("luci.model.uci").cursor()
print(type(c:get("netmon",  "main", "listen_addr")))  -- string  "lan"
print(type(c:get("network", "lan",  "ipaddr")))       -- table   { "192.168.1.1/24" }
uci -q get network.lan.ipaddr                          -- 192.168.1.1/24（CLI 是字符串）
```

`network.lan.ipaddr='192.168.1.1/24'` 在 UCI 里是标量，CLI 读出来是字符串，
但走 LuCI 的 ucode 桥接后变成 `{ [1] = "192.168.1.1/24" }`。
于是 `addr:gsub` 变成「对 table 取 gsub 字段」→ nil → 调用 nil → 500，
LuCI 显示 `Runtime error`。

**教训**：在 LuCI 模板里读 UCI 必须做类型归一化，
不能假设 `get()` 返回字符串：

```lua
local function flat(v)
    if type(v) == "table" then v = v[1] end
    return (v ~= nil) and tostring(v) or ""
end
```

### 修法：模板里彻底不读 UCI

面板只需要同源路径 `/netmon/`，端口和域名都不参与，所以
`view/netmon/index.htm` 现在**没有任何动态 Lua 逻辑**——只有
`<%+header%>`、纯 HTML、`<%:翻译%>`、`<%+footer%>`。
根因随之消失，也不会因为 UCI 结构变化再炸。

新增离线验证工具 `build/verify-luci-view.lua`，用 LuCI 自己的模板编译器
（`luci.template.parser.parse(file)`，与 `template.lua` 内部调用方式一致）
检查模板能否编译：

```bash
lua build/verify-luci-view.lua /usr/lib/lua/luci/view/netmon/index.htm
#   COMPILE_OK  ...
```

故意写坏的模板会得到 `COMPILE_FAIL Syntax error ...`，说明这个检查有效。

### 关于 `router.example.lan:8090` 打不开

这不是 bug，是**架构决定**：

| 条件 | 现状 |
|---|---|
| Caddy 监听 | 只有 443（反代到 uhttpd `127.0.0.1:8080`），**没有 8090** |
| 独立 uhttpd 实例 | 绑定 `192.168.1.1:8090`，只监听 LAN IP，域名解析到它也没服务 |
| 因此 | `https://router.example.lan:8090/` 必然连不上 |

面板现在**不再需要 8090**：它走 LuCI 同源路径 `/netmon/`，
随 LuCI 一起用 80/443。正确入口：

```
https://router.example.lan/cgi-bin/luci/admin/status/netmon
或 LuCI 菜单：状态 → 监控面板
```

8090 仅保留给「LAN 内直连」使用：`http://192.168.1.1:8090/`。
模板里那个会指向 8090 的「直连独立端口」链接已删除，避免再次误导。


---

## 2026-11-08 结论：没有 Caddy 的默认 ImmortalWrt 能用吗？

**能，而且比这台机器更省事——不需要改任何 Web 服务器配置。**

原因：面板不依赖 Caddy。它依赖的是 **uhttpd 主实例的两个默认能力**：

| 依赖 | 默认值 | 谁提供 |
|---|---|---|
| `/netmon/` 静态页面 | `uhttpd.main.home='/www'` | uhttpd 主实例（默认配置就是这个） |
| `/cgi-bin/netmon-data` 执行 | `uhttpd.main.cgi_prefix='/cgi-bin'` | 同上 |
| iframe 同源嵌入 | uhttpd 默认**不发** `X-Frame-Options`、不发 CSP | 同上 |

Caddy 在这台机器上只是做 TLS 终止和反代，**不是插件的前置条件**。

### 实测（绕过 Caddy，直连 uhttpd）

```
GET http://127.0.0.1:8080/netmon/              → 200  145722 B
GET http://127.0.0.1:8080/cgi-bin/netmon-data  → 200   74228 B
响应头                                          → 无 X-Frame-Options / CSP
```

这台机器的 uhttpd 监听被改成了 `127.0.0.1:8080`（因为 Caddy 占 443）。
默认 ImmortalWrt 下它是 `0.0.0.0:80`，**同样是这套 home/cgi_prefix**，
所以 `/netmon/` 与 CGI 的表现完全一致。

### 两类环境对照

| 项目 | 默认 ImmortalWrt（uhttpd 直连） | 本机（Caddy 反代） |
|---|---|---|
| 面板入口 | `http://<路由器IP>/cgi-bin/luci/admin/status/netmon` | `https://router.example.lan/cgi-bin/luci/admin/status/netmon` |
| `/netmon/` 提供者 | uhttpd 主实例 | uhttpd 主实例（经 Caddy 转发） |
| iframe 是否需要额外放行 | **不需要**（uhttpd 不发 XFO） | **需要**（Caddy 全站 `X-Frame-Options: DENY`，已对 `/netmon/*` 放开） |
| 是否需要改 Web 配置 | **不需要** | 需要那一段 Caddy 例外 |

### 一个前提条件

`cgi_prefix` 必须是默认的 `/cgi-bin`（OpenWrt / ImmortalWrt 出厂值）。
若有人改成别的值，`/cgi-bin/netmon-data` 会被当作静态文件而不是 CGI 执行。
这种情况需要在 `netmon-apply` 里把 CGI 复制到实际前缀目录，
并把前端 `apiBase()` 的路径一起改——属于非常规配置，未做自动适配，
文档在此标注。

### 独立 8090 实例在默认环境下的意义

它依然会被 uci-defaults 创建（`listen_addr` 默认 `lan`），作用是
「不经过 LuCI 直接看面板」。默认环境里 80 端口空闲时它也无害；
不想用时把 `netmon.main.http_port` 改掉或删掉 `uhttpd.netmon` 段即可，
**不影响 LuCI 内的面板**。

---

## 2026-11-08 三：去掉 LuCI 内嵌面板（1.0.0-r2）

### 为什么去掉

面板是独立单页应用，按自己的视口做响应式。塞进 LuCI 的 iframe 后只有两层约束：

* **高度**：LuCI 内容区本身已经扣掉 header/footer，iframe 再取 76vh，
  实际可视高度不到 600px，图表被压扁、榜单只能看到两三行。
* **宽度**：主题把内容区限制在固定宽度，多列网格被迫折成单列；
  移动端还要在 iframe 里再横向滚动一次，双滚动条很难用。

结论：面板只在独立页面里呈现，LuCI 只承担「配置 + 入口」。

### 改了什么

| 文件 | 变化 |
|---|---|
| `luasrc/controller/netmon.lua` | 只剩一个 entry：`admin/status/netmon` → `cbi("netmon/settings")`，`.leaf = true`。不再有 `panel` / `settings` 两个子节点，侧栏点「监控面板」直接进设置页，也不会出现只有一项的 tab 条 |
| `luasrc/model/cbi/netmon/settings.lua` | 页头 description 里加「打开监控面板」按钮（`href="/netmon/"`，新标签）+ 一行独立端口地址提示 |
| `luasrc/view/netmon/index.htm` | **删除**。升级时 apk 会自动清掉旧文件 |
| `Makefile` / `build/build-apk.py` | 安装清单从 3 个 Lua 文件降到 2 个 |

### 页头按钮为什么放在 map description 里

经典 CBI 的 `map.htm` / `nsection.htm` 用 `<%=self.description%>` 原样输出，
**不经过 pcdata 转义**，所以可以直接写 `<a class="cbi-button">`。
试过 `DummyValue + rawhtml`，也能出按钮，但 `dvalue.htm` 会额外把整段 HTML
写进隐藏 input 的 value（转义后满是 `&#60;`），源码很脏；而且 CBI 只允许
一个 option 属于一个 section，为了一个链接再开一个同名 NamedSection 也没必要。

### 端口提示怎么来的

`luci.model.uci` 的 `uci:get` 在新版是 ucode 桥接，标量也可能返回 table，
一 `gsub` 就 500（本项目已经踩过一次）。所以这里统一走
`uci -q get <key>` + `pcall`，只取纯字符串：

```lua
local function uci_get(opt)
    local ok, out = pcall(function() return sys.exec("uci -q get " .. opt .. " 2>/dev/null") end)
    if not ok or type(out) ~= "string" then return "" end
    return (out:gsub("%s+$", ""))
end
```

`listen_addr=lan` 时取 `network.lan.ipaddr` 并去掉 `/24` 掩码，其余原样；
取不到就退回 `127.0.0.1`，端口取不到退回 8090——任何一项读失败都只是提示文字
不精确，不会让页面报错。

### 实测（升级安装后）

```
apk add --allow-untrusted /tmp/netmon-1.0.0-r2.apk /tmp/luci-app-netmon-1.0.0-r2.apk
  (1/2) Upgrading netmon (1.0.0-r1 -> 1.0.0-r2)
  (2/2) Upgrading luci-app-netmon (1.0.0-r1 -> 1.0.0-r2)

apk info -L luci-app-netmon
  usr/lib/lua/luci/controller/netmon.lua
  usr/lib/lua/luci/model/cbi/netmon/settings.lua      # 旧 view 已被 apk 清除

/admin/status/netmon        200   9633 B   无 iframe、无 tab 条、按钮 + 端口提示
/admin/menu/status 菜单树   单一节点：title=监控面板 → invoke_cbi_action(netmon/settings)
/netmon/  (8080 同源)       200  145722 B
http://192.168.1.1:8090/      200  145722 B
/netmon/cgi-bin/data        200    4853 B
```

### 顺带修的两处

1. `build-apk.py` 的前端版本戳正则原为 `页面版本 (\S+?)<`，
   而实际文本是 `页面版本 2026-11-08d ·`，中间隔着 `·`，
   于是每次构建都打印「未找到」，防漂移断言形同虚设。改为只取版本号本身。
2. `/etc/config/netmon.apk-new` 残件：实测 apk **在 post-install 跑完之后**才落盘这个
   文件，所以包脚本里 `rm` 会扑空；后台 `(sleep 2; rm) &` 也会随脚本进程被回收。
   最终把清理点放在 `/etc/init.d/netmon` 的 `start_service()` 里——每次启动
   （包括 `netmon-apply` 触发的重启）扫一次，装完第一次重启或下次开机即清干净。
   实测：装完当场文件在，`/etc/init.d/netmon restart` 后消失。

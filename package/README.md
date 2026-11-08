# LuCI 应用分发包骨架

两个包，均按 OpenWrt/ImmortalWrt SDK 的 `package/` 目录布局写好，
**本机不编译**（交叉编译必须由目标 SDK 完成）。

| 目录 | 产物 | 作用 |
|---|---|---|
| `netmon/` | `netmon_*.apk` / `.ipk` | 采集与报表二进制、procd 服务、UCI 配置、`/www/netmon` 页面与 CGI、uhttpd 实例 |
| `luci-app-netmon/` | `luci-app-netmon_*.apk` | LuCI「状态 → 监控面板」：CBI 设置页 + 面板入口按钮（面板本身独立成页） |

## 依赖声明

`netmon` 的 `DEPENDS` 只列运行必需的组件：`ip-full`、`uhttpd`、`kmod-nf-conntrack`。
SQLite 以 amalgamation **静态链接**进二进制（`src/get-sqlite.sh` 生成），
所以运行时**不需要** `libsqlite3-0`；主动探测停用后也不再需要 `curl`。

**homeproxy / sing-box 刻意不写进 DEPENDS**：它们是可选增强。
未安装时 `hprules_init()` 安静降级，`hp.available=0`，界面显示说明卡，
设备、流量、DNS 日志、系统状态全部照常工作。装好后 30 秒内自动启用拦截归因，
不需要改代码或重启。

## 源码位置

`netmon/Makefile` 直接从仓库根目录取源码与前端，**包内不保留副本**：

```
$(CURDIR)/../../src   →  $(PKG_BUILD_DIR)/src     编译
$(CURDIR)/../../www   →  $(1)/www/netmon/         安装
```

早期 `./files/src`、`./files/index.html` 各存了一份拷贝，结果出现过
「包里前端落后源码两个版本」的漂移事故（docs/PACKAGING.md 有完整记录）。
改成单源之后这类问题从结构上消失。

唯一的例外是 `files/uhttpd-netmon`（uci-defaults），它只属于包，不参与运行时代码。

编译前先生成 SQLite 静态库：

```bash
sh src/get-sqlite.sh          # → build/sqlite/libsqlite3.a
```

`files/uhttpd-netmon` 是 uci-defaults，安装时按 `/etc/config/netmon` 里的
`http_port` / `listen_addr` 创建独立 uhttpd 实例（默认把 `lan` 解析成 LAN 地址 + `8090` 端口）。
**端口不再写死**：之后改端口走 LuCI 设置页，由 `/usr/bin/netmon-apply` 重写实例并 reload，
不需要重装包。脚本全文见 `package/netmon/files/uhttpd-netmon`。

## 编译与验证

```bash
# 在 SDK 根目录
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig       # Network -> netmon；LuCI -> Applications -> luci-app-netmon
make package/netmon/compile V=s
make package/luci-app-netmon/compile V=s
```

装机后检查：`/etc/init.d/netmon status`、浏览器打开 `:8090`、
LuCI 菜单能打开 iframe、未装 homeproxy 时「拦截记录」显示降级说明。

## 已知边界

- 默认 30 天明细保留约 700 MB，小 Flash 机型必须外接存储或缩短 `retain`。
- iframe 方式与 LuCI 的 ACL 是两套，若需统一登录鉴权，应改成 rpcd/ubus 接口
  （工作量约 3–5 天，见 `docs/PACKAGING.md`）。


---

## 2026-11-08 更新：LuCI 设置页与可配置项

对标 `luci-app-adguardhome`，把常用设置做成真正能改的 UCI 项，
**保存即生效**，不需要手工编辑配置文件。

| 设置项 | UCI 键 | 默认值 | 生效方式 |
|---|---|---|---|
| 启用监控 | `enabled` | `1` | 重启采集进程 |
| 面板端口 | `http_port` | `8090` | `netmon-apply` 重写 `uhttpd.netmon.listen_http` 并 reload |
| 监听地址 | `listen_addr` | `lan` | 自动取 `network.lan.ipaddr`；也可填具体 IP |
| 数据库位置 | `db_path` | `/opt/netmon/data/netmon.db` | 建目录 + 重启采集；**历史数据不自动迁移** |
| 明细保留天数 | `retain` | `30` | 重启采集（init.d 传 `-r`） |
| 采样间隔 | `interval` | `5` 秒 | 重启采集（init.d 传 `-i`） |
| 调试日志 | `loglevel` | `0` | 重启采集（追加 `-v`） |

### 文件布局

```
package/netmon/files/
  config-netmon          UCI 默认配置（含上述 7 项）
  init.d-netmon          procd 服务，PROG=/usr/bin/netmon
  netmon-apply           配置应用脚本（装到 /usr/bin/netmon-apply）
  uhttpd-netmon          uci-defaults，安装时建独立 Web 实例
package/luci-app-netmon/luasrc/
  controller/netmon.lua  菜单：状态 → 监控面板（直达设置页）
  model/cbi/netmon/settings.lua   CBI 设置页 + 「打开监控面板」按钮
```

1.0.0-r2 起不再包含 `view/netmon/index.htm`：iframe 内嵌面板受 LuCI 主题的
高度/宽度限制，桌面端只剩约 60vh、移动端还要横向滚动，观感明显不如独立页面。
面板本身仍在 `/netmon/`（uhttpd 主实例同源提供）与独立端口两处可直接访问，
设置页顶部给了一个新标签打开的按钮。

`netmon-apply` 是幂等的，可手动执行，也由 CBI 的 `on_after_commit` 在后台调用：
校验端口 → 建数据库目录 → 重写 uhttpd 实例 → reload uhttpd → 重启采集。
它只改 `uhttpd.netmon`，不动 LuCI 主实例、dnsmasq、homeproxy 与防火墙。

### 依赖与本机实测

- `luci-app-netmon` 的 `LUCI_DEPENDS` 加了 `+luci-compat`（CBI 框架）。
  该包在 ImmortalWrt 25.12 官方 apk 仓库中，本机已安装；
  `/usr/lib/lua/luci/{cbi,model/uci,sys,dispatcher}.lua` 齐全，
  `lua -e 'require("luci.model.uci")'` 在路由器上返回 OK。
- `luac -p` 检查 controller 与 CBI 设置页，语法通过；`sh -n` 检查 init.d、
  `netmon-apply`、`uhttpd-netmon`、`config-netmon` 全部通过。
- 在现有路由器上实测执行 `/usr/bin/netmon-apply`：输出正常、uhttpd 监听保持
  `192.168.1.1:8090`、采集进程以 `exe=/usr/bin/netmon` 重新启动、页面与三个接口全 200。
- 修掉一个打包隐患：旧 `init.d` 里 `PROG=/opt/netmon/src/netmon`，
  而包把二进制装到 `/usr/bin/netmon`，打包后服务会直接起不来。已改为 `/usr/bin/netmon`。
- `Package/netmon/conffiles` 声明了 `/etc/config/netmon`，升级包时保留用户设置。

### 仍待完成（评估边界）

1. **交叉编译实测**：本机是 x86，其它架构（MT7621 / MT7981 / MT7986 等）
   必须用目标 SDK 编译验证，尚未做。
2. **设置页浏览器实测**：CBI 页已写完并通过语法检查，但只有装包后才能在 LuCI 里点验。
3. **鉴权**：iframe 与 LuCI 是两套会话，独立端口无鉴权。分发时应默认只监听 LAN，
   或加防火墙规则；要统一登录需改造成 rpcd/ubus（约 3–5 天）。
4. **HTTPS 混合内容**：LuCI 走 HTTPS 时浏览器会阻止嵌入 HTTP 面板，
   view 里已给「在新窗口打开」链接。


---

## 2026-11-08：apk 产物已生成

`netmon/dist/` 下有两个可直接安装的包，构建脚本是 `build/build-apk.py`：

```bash
python3 build/build-apk.py        # 生成 dist/*.apk
apk add --allow-untrusted dist/netmon-1.0.0-r1.apk dist/luci-app-netmon-1.0.0-r1.apk
```

包是 apk-tools 3 的原生 **ADB v3** 格式，由 SDK 的 `apk mkpkg` 生成；
二进制用目标机 `x86_64-openwrt-linux-musl gcc` 编译。
依赖全部来自官方仓库，详见 `docs/PACKAGING.md` 的实测章节。

`package/` 下两个 Makefile 是给完整 buildroot `make package/.../compile` 用的骨架；
本次 x86 单平台打包走的是更直接的 `mkpkg` 路径，两条路径产出的文件布局一致。

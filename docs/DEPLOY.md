# netmon 部署文档

从零在一台 ImmortalWrt 25.12 上装起 netmon 的完整步骤。

已验证环境：
- ImmortalWrt 25.12 / x86-64 / musl
- 路由器地址 `192.168.1.1`
- WAN 接口 `pppoe-wan`，IP `203.0.113.45`（PPPoE 动态，会变）

---

## 0. 前置：登录与传文件

### SSH 登录

```bash
# 首次用密码登录，然后装公钥
ssh root@192.168.1.1
# 密码：见密码管理器
```

装公钥免密：
```bash
# 本地生成（如没有）
ssh-keygen -t ed25519 -f ~/.ssh/id_netmon -N ''

# 推公钥到路由器
ssh-copy-id -i ~/.ssh/id_netmon.pub root@192.168.1.1
```

### 建独立 SSH 配置

> **注意**：这台机器上 `/usr/lib/systemd/ssh_config.d/*.conf` 属主是 `nobody`，
> 默认 `ssh` 会直接退出。必须用 `-F` 指定一个干净的配置文件绕开。

`~/.ssh/netmon.conf`：
```
Host r
    HostName 192.168.1.1
    User root
    IdentityFile ~/.ssh/id_netmon
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
```

之后所有命令：
```bash
ssh -F ~/.ssh/netmon.conf r "<命令>"
```

### 传文件

路由器**没有 sftp-server**，`scp` 不可用。用 tar 管道：

```bash
tar cf - 文件1 文件2 | ssh -F ~/.ssh/netmon.conf r 'cd /目标目录 && tar xf -'
```

### 注意 `/tmp` 会被重置

每次 SSH 命令执行的 `/tmp` 可能是干净的。临时文件放 `$HOME/netmon-work/` 或项目目录。

---

## 1. 编译 SQLite 静态库

> 为什么要静态编译：系统 `libsqlite3.so` 被 strip 掉了 section headers，
> gcc 链接时报 `file in wrong format`；且 apk 仓库没有 sqlite 开发包。
> 详见 `DESIGN.md` 第 3.1 节。

```bash
# 在路由器上
mkdir -p /opt/netmon/{src,include,data,sqa}

cd /opt/netmon/sqa
# 下载 amalgamation（3.50.3）
# 注意：直连 GitHub 很慢，分段下载或走镜像更快
wget https://www.sqlite.org/2025/sqlite-amalgamation-3500300.zip

unzip sqlite-amalgamation-3500300.zip
cd sqlite-amalgamation-3500300

# 编译静态库
gcc -O2 -c sqlite3.c -o sqlite3.o -DSQLITE_ENABLE_FTS5=0 -DSQLITE_OMIT_LOAD_EXTENSION
ar rcs libsqlite3.a sqlite3.o

cp sqlite3.h /opt/netmon/include/
ls -la libsqlite3.a       # 应为 1.3MB 左右
```

**下载踩坑记录**：直连 507KB/300s、走代理 950KB，最后分段下载（11 片 × 256KB）
花了 6 分 47 秒才成功。GitHub 镜像 591KB/s 更快。校验 `SIZE_OK` = 2845433 字节。

---

## 2. 编译 netmon

```bash
# 上传源码（从本地项目目录）
cd /path/to/netmon/src
tar cf - *.c *.h Makefile init.d-netmon config-netmon \
  | ssh -F ~/.ssh/netmon.conf r 'cd /opt/netmon/src && tar xf -'

# 编译
ssh -F ~/.ssh/netmon.conf r 'cd /opt/netmon/src && make'
```

如果 `make` 不可用，手动编译：
```bash
cd /opt/netmon/src
gcc -O2 -Wall -o netmon \
    netmon.c probe.c db.c ctsample.c ctparse.c dnsmap.c dnssniff.c \
    -I/opt/netmon/include \
    -L/opt/netmon/sqa/sqlite-amalgamation-3500300 \
    -lsqlite3

gcc -O2 -Wall -Wextra -o netmon-report netmon-report.c \
    -I/opt/netmon/include \
    -L/opt/netmon/sqa/sqlite-amalgamation-3500300 \
    -lsqlite3
```

> **不要加 `-lpthread`** —— musl 已内置线程支持，加了会报
> `cannot find -lpthread`。

编译产物：
- `netmon` — 采集守护进程（约 1.1MB）
- `netmon-report` — 查询工具（约 1.1MB）

---

## 3. 安装

```bash
ssh -F ~/.ssh/netmon.conf r '
  mkdir -p /opt/netmon/data
  cp /opt/netmon/src/netmon        /usr/bin/netmon
  cp /opt/netmon/src/netmon-report /usr/bin/netmon-report
  chmod 755 /usr/bin/netmon /usr/bin/netmon-report

  cp /opt/netmon/src/init.d-netmon /etc/init.d/netmon
  chmod 755 /etc/init.d/netmon

  cp /opt/netmon/src/config-netmon /etc/config/netmon
'
```

### 服务脚本做了什么

`/etc/init.d/netmon`（procd）：
- `START=95` —— 晚于网络（S95 之前）启动，确保 WAN 已就绪
- **自动探测 WAN IP**：优先 `pppoe-wan`，退回默认路由接口
  （PPPoE 每次重拨 IP 会变，不能写死）
- `procd_set_param respawn 3600 5 5` —— 崩溃后 5 秒重启，1 小时内最多 5 次
- 退出时发 `SIGTERM`，让程序做最后一次 `db_flush()`

### UCI 配置项

`/etc/config/netmon`：

| 选项 | 默认 | 说明 |
|---|---|---|
| `enabled` | 1 | 是否启用 |
| `db_path` | `/opt/netmon/data/netmon.db` | 数据库路径 |
| `interval` | 5 | 采样间隔（秒） |
| `retain` | 30 | 明细保留天数 |
| `probe` | 1 | 是否启用主动探测 |
| `loglevel` | 1 | 0=静默 1=正常 2=详细 |

修改后：
```bash
uci set netmon.main.interval=10
uci commit netmon
/etc/init.d/netmon restart
```

---

## 4. 启动

```bash
ssh -F ~/.ssh/netmon.conf r '/etc/init.d/netmon enable && /etc/init.d/netmon start'
```

验证：
```bash
ssh -F ~/.ssh/netmon.conf r '/etc/init.d/netmon status'
# 期望输出：running

ssh -F ~/.ssh/netmon.conf r 'logread -e netmon | tail -20'
```

正常启动日志：
```
netmon: started: db=/opt/netmon/data/netmon.db interval=5s retain=30d wan=203.0.113.45
探测绕过: interface=203.0.113.45
netmon 启动: db=/opt/netmon/data/netmon.db 采样=5000ms 保留=30天
```

约 20 秒后第一轮探测完成，会看到：
```
probe oversea    104.16.0.1       loss=0% avg=238.1ms jitter=0.3
probe bw proof.ovh.net    1426 kbps (1.00 MB / 5.9s)
probe bw mirrors.aliyun.com 211898 kbps (4.00 MB / 0.2s)
dns www.baidu.com        rcode=0 0.7ms answers=4
```

---

## 5. 部署 Web 前端

### 5.1 拷文件

```bash
# 从本地项目目录
cd netmon/www
tar cf - index.html cgi-bin-data \
  | ssh -F ~/.ssh/netmon.conf r 'cd /www/netmon && tar xf - && \
      mv cgi-bin-data cgi-bin/data && \
      chmod 755 cgi-bin/data && chmod 644 index.html'
```

目录结构：
```
/www/netmon/index.html       页面
/www/netmon/cgi-bin/data     CGI 接口（755）
```

### 5.2 加 uhttpd 独立实例

**先备份**（重要）：
```bash
ssh -F ~/.ssh/netmon.conf r 'cp /etc/config/uhttpd /root/uhttpd.bak'
```

然后加配置：
```bash
ssh -F ~/.ssh/netmon.conf r '
uci set uhttpd.netmon=uhttpd
uci set uhttpd.netmon.home="/www/netmon"
uci set uhttpd.netmon.listen_http="192.168.1.1:8090"
uci set uhttpd.netmon.cgi_prefix="/cgi-bin"
uci set uhttpd.netmon.index_page="index.html"
uci set uhttpd.netmon.max_requests="10"
uci set uhttpd.netmon.max_connections="30"
uci set uhttpd.netmon.script_timeout="20"
uci set uhttpd.netmon.network_timeout="20"
uci set uhttpd.netmon.no_symlinks="1"
uci set uhttpd.netmon.rfc1918_filter="0"
uci commit uhttpd
/etc/init.d/uhttpd reload'
```

> **安全要点**：`listen_http` 必须是 `192.168.1.1:8090`（只绑 LAN IP），
> **不能是 `0.0.0.0:8090`**，否则会暴露到 WAN。

### 5.3 验证

```bash
ssh -F ~/.ssh/netmon.conf r '
netstat -tlnp | grep -E "8090|8080"
curl -s -o /dev/null -w "page=%{http_code}\n" http://192.168.1.1:8090/
curl -s "http://192.168.1.1:8090/cgi-bin/data?since=86400" | head -c 200'
```

期望：
- `192.168.1.1:8090` 在监听（新增）
- `127.0.0.1:8080` 仍在监听（LuCI，不受影响）
- `page=200`、接口返回 JSON

浏览器打开 **http://192.168.1.1:8090/**

> `uhttpd reload` 只重载配置，LuCI 连接不断，也不影响 caddy 和网络。

---

## 6. 查询

`netmon-report` 已装到 `/usr/bin`：

```bash
# 数据库概况
netmon-report status

# 流量排行（默认按目的网段，最近 24h）
netmon-report top
netmon-report top --since 7d -n 30
netmon-report top --by host          # 按域名（需启用 DNS 反查）
netmon-report top --by port          # 按端口

# 时间趋势
netmon-report trend --since 7d --unit day
netmon-report trend --since 24h --unit hour

# 探测质量
netmon-report probe --since 24h
netmon-report probe --target 104.16.0.1

# DNS 质量
netmon-report dns --since 7d

# JSON 输出（给脚本用）
netmon-report top --since 24h -j
```

---

## 7. 卸载 / 回滚

```bash
ssh -F ~/.ssh/netmon.conf r '
  /etc/init.d/netmon stop
  /etc/init.d/netmon disable
  rm -f /etc/init.d/netmon /etc/config/netmon
  rm -f /usr/bin/netmon /usr/bin/netmon-report
'
# 数据保留在 /opt/netmon/data，需要彻底删除再 rm -rf /opt/netmon
```

**注意**：停掉 netmon **不影响任何网络功能**。它只读 conntrack、只发探测包，
不改路由/防火墙/DNS。这一点在 `OPERATIONS.md` 事故复盘里也确认过。

---

## 8. 检查清单

部署完逐项确认：

**采集与存储**
- [ ] `netmon-report status` 能看到数据
- [ ] `stat:` 行的"采样"耗时 < 100µs（debug 级，需 `loglevel=1`）
- [ ] `/etc/init.d/netmon status` 是 `running`
- [ ] 重启路由器后服务自动起来（`enable` 已生效）

**探测**
- [ ] `probe bw proof.ovh.net` 不是 FAILED
- [ ] `probe bw mirrors.aliyun.com` 不是 FAILED
- [ ] `netmon-report probe` 里没有 100% 丢包的可疑目标
- [ ] `dns` 报表里 `www.baidu.com` 耗时 < 10ms

**DNS 反查**
- [ ] 日志里有 `DNS 嗅探已启动: 监听 br-lan`
- [ ] `netmon-report dns` 能看到真实域名（不是只有 netmon 自己探测的那几个）
- [ ] `netmon-report top --by host` 有输出
- [ ] `logread -e netmon | grep 嗅探` 没有反复重启的日志

**Web 前端**
- [ ] `netstat -tlnp | grep 8090` 显示 `192.168.1.1:8090`（**不是 0.0.0.0**）
- [ ] 浏览器能打开 http://192.168.1.1:8090/
- [ ] 页面有数据，圆点是绿色
- [ ] LuCI (http://192.168.1.1) 仍正常
- [ ] caddy 站点（blog.example.lan 等）仍正常

**底线（最重要）**
- [ ] 局域网设备上网正常（`ping 223.5.5.5` 通）
- [ ] DNS 解析正常（`nslookup www.baidu.com 192.168.1.1` 有结果）
- [ ] 停掉 netmon 后网络依然正常（`/etc/init.d/netmon stop` 验证）
- [ ] 再启动回来（`/etc/init.d/netmon start`）

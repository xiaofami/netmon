# 性能优化记录（2026-11-07）

## 优化前基线

x86_64 软路由、24h 窗口约 43 万条 DNS 记录 / 20 万条连接记录：

| 指标 | 优化前 |
|---|---|
| `netmon-report dashboard` 单次 | **3.2 – 4.0 秒** |
| 前端自动刷新 | 每 10 秒发一次，持续占用约 1/3 个 CPU 核 |
| 主要瓶颈 | 十多条 `FROM dnsq WHERE ts_ms>=?` 全窗口聚合，多次回表 |

## 本轮措施

### 1. 复合覆盖索引（永久生效）

```sql
CREATE INDEX idx_dnsq_tc ON dnsq(ts_ms, client_ip);
CREATE INDEX idx_dnsq_tq ON dnsq(ts_ms, qname);
CREATE INDEX idx_dnsq_tr ON dnsq(ts_ms, rcode, qname);
CREATE INDEX idx_conns_ts ON conns(first_ms, src);
```

`EXPLAIN QUERY PLAN` 验证已走覆盖索引，不再回表：

```
SEARCH dnsq USING COVERING INDEX idx_dnsq_tc (ts_ms>?)
SEARCH dnsq USING COVERING INDEX idx_dnsq_tr (ts_ms>?)
```

索引建在 `db_init_schema()`，新装自动创建；存量库由部署脚本幂等补建。

### 2. CGI 结果缓存（收益最大）

`www/cgi-bin-data` 对 dashboard 做 **10 秒窗口缓存**：

- 文件名带 `$(date +%s)/10` 窗口编号，同窗口直接 `cat` 返回，跨窗口自动失效；
- 不用 `stat`（**busybox 没有这个命令**，第一版踩过坑）；
- 写临时文件 + 原子 `mv`，避免并发刷新读到半个 JSON；
- 按 `since` 分文件，切换时间范围立即失效；client/detail 请求不缓存；
- **HTTP 头必须在所有分支之前打印**（缓存命中会提前 `exit 0`），
  否则 uhttpd 收不到头直接返回 502；
- 清理旧缓存用显式 `if`，空目录时 `&&` 链返回 1 会被 `set -e` 静默退出。

### 3. 去掉停用模块的扫描

隐私观察、服务类别、线路质量、代理探测四块 SQL 与 `/proc` 遍历全部移除，
dashboard 顶层字段从 17 个减到 15 个。

### 4. 采集侧

采集进程不再跑 ping / HTTP / 代理探测，省掉每 5 分钟一次的出站流量与
每 15 分钟一轮的双路径 curl。规则指纹同步（30 秒）保留，因为拦截归因需要它。

## 实测结果

| 指标 | 优化后 |
|---|---|
| CGI 缓存命中 | **28 – 29 ms**（约 **140 倍**） |
| 冷请求（报表自身） | 3.97 / 4.03 s，已被 10 秒窗口摊薄到极低频率 |
| 索引 | 4 个覆盖索引全部命中 |
| 采集进程 CPU | 单核 **0.08%** |
| RSS | 83.6 MB（主要来自 64 MB SQLite 页缓存，可调） |

前端每 10 秒刷新一次时，绝大多数请求落在缓存上；只有跨窗口的第一个请求
会真正跑聚合，且结果被同窗口内其它标签页复用。

## 后续可继续优化（按性价比）

1. **预生成缓存**：让采集进程每 30 秒后台写一次 dashboard JSON，
   彻底消除用户侧的冷请求尖峰（需处理并发与内存）。
2. **减少重复扫描**：`summary` 与 `devices` 段仍各自扫一次 dnsq/conns，
   可合并为一次带多个聚合的扫描。
3. **按窗口预聚合**：`agg_hour` 已存在，长时段（7 天/30 天）改读聚合表，
   避免窗口一大就线性变慢。
4. **低资源机型**：把 `cache_size` 从 64 MB 调到 8 MB、保留期从 30 天
   降到 3–7 天，可把 RSS 压到 25–30 MB、数据库控制在 100 MB 内。

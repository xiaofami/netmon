# 文档索引

按「你想干什么」找文档：

| 你想 | 看 |
|---|---|
| 装上并跑起来 | [DEPLOY.md](DEPLOY.md) —— 从零在一台 ImmortalWrt 上装起 |
| 日常维护、出问题怎么查 | [OPERATIONS.md](OPERATIONS.md) —— 运维手册与**必须避免的操作** |
| 搞懂为什么这么设计 | [DESIGN.md](DESIGN.md) —— 形态选择、数据模型、性能取舍 |
| 域名是怎么反查出来的 | [DNS_REVERSE.md](DNS_REVERSE.md) —— 四种方案对比与 AF_PACKET 的选择理由 |
| 和 homeproxy 怎么配合 | [HOMEPROXY.md](HOMEPROXY.md) —— 规则同步与被动拦截归因 |
| 服务识别是怎么分的 | [APPS.md](APPS.md) —— 三层元数据特征，不抓包 |
| 面板怎么改 | [WEB.md](WEB.md) —— 架构、部署、接口字段、排查 |
| 想换外观 | [THEMES.md](THEMES.md) —— 6 套皮肤怎么加 |
| 依赖、资源、能装什么机型 | [PACKAGING.md](PACKAGING.md) —— 依赖盘点、实测占用、apk 构建 |

## 这里没有什么

- **开发过程日志**。逐次迭代、性能优化前后对比、功能取舍的讨论记录，都不在仓库里 ——
  公开仓库留的是「现在是什么样、为什么」，不是「当时怎么一步步试出来的」。
- **真实数据**。所有示例地址、设备名、域名都是编的。仓库有
  `build/check-leak.py` 做自动检查，CI 里会拦。
- **更新日志**。版本变化见 [Releases](https://github.com/xiaofami/netmon/releases)。

## 改文档的约定

- 示例地址用 `192.168.1.x`，私有网段示例用 `192.168.1.0/24` / `198.18.0.0/16`，
  文档保留地址用 `192.0.2.x`（RFC 5737）、`2001:db8::/32`（RFC 3849）
- 提到源文件时写真实路径，`build/check-leak.py` 之外没有自动校验，改代码记得改文档
- 不写「本轮」「本次迭代」这类相对时间的表述 —— 读者不知道「本轮」是哪一轮

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""隐私自检：仓库里不允许出现真实的内网地址、真实主机名。

为什么单独做一个检查
--------------------
这个项目自己就是「看别人上网」的工具，所以对隐私格外要小心。之前踩过两次：

1. `ignore_conn()` 里用字节比较写死了开发机的 LAN 网段，跟着代码进了公开仓库
   （文本正则 `10\\.89\\.2\\.` 匹配不到 `s[1]==89 && s[2]==2` 这种形式）
2. 1.0.6 的 release note 为了说明 tun 代理的重复计量，直接贴了两行真实
   conntrack —— 客户端 LAN 地址 + 本机 PPPoE WAN 地址

两次都不是写入时故意的，是「顺手举例」和「按文本形式查漏了」。
所以这里按「形式」检查，不依赖记得去 grep。

检查范围：所有 git 已跟踪的文本文件。未跟踪的本地文件（如
www/preview-live.html、tools/*.json）本来就不会被提交，不在范围内。

退出码非 0 时 CI 会红，阻止发布。
"""
import re
import subprocess
import sys

# (正则, 说明, 是否允许出现在示例里)
RULES = [
    # 私有地址：示例统一用 192.168.1.x / 192.168.2.x（见 make-demo.py 的约定）
    (r"\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b", "私网 10/8", False),
    (r"\b172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b", "私网 172.16/12", False),
    (r"\b192\.168\.(?!1\.|2\.)\d{1,3}\.\d{1,3}\b", "非示例 192.168 段", False),

    # 源码里把 IP 拆成字节比较：s[0]==10 && s[1]==89
    (r"\[\s*0\s*\]\s*==\s*(?:10|172|192)\b", "疑似字节比较的硬编码网段", False),

    # 主机名形态
    (r"\bDESKTOP-[A-Z0-9]{5,}\b", "Windows 默认主机名", False),
    (r"\b(?:OPPO|NOVA|MIX|GEN8|UGOS|WTA|ILOCZ)-?[A-Z0-9]{2,}\b", "疑似厂商型号", False),
    (r"\b[A-Za-z][A-Za-z0-9]*-mibt[A-Z0-9]{4}\b", "小米设备后缀", False),

    # 个人环境痕迹：家目录里自建的临时目录、自命名密钥
    # （文档里应该写 ~/.ssh/config 这种通用路径）
    (r"~/[a-z0-9]{2,12}/(?:netmon.conf|id_\w+)", "个人自建 SSH 目录/密钥名", False),
    (r"\$HOME/[a-z0-9]{2,12}/netmon.conf", "个人自建 SSH 配置路径", False),

    # 固网/移动的真实 IPv6 前缀：2409 中国移动、2408 联通、240e 电信
    (r"\b240[89e]:[0-9a-f]{1,4}:", "真实运营商 IPv6 前缀", False),
]

# 整文件豁免：本检查器自己列了这些正则
ALLOW_FILES = {
    "build/check-leak.py",
}

# 行级豁免：某些看起来像「型号」的字符串其实是功能的一部分。
# 设备分类要认市售型号（TP-LINK WTA301、NETGEAR GS116E 等），这些是
# 公开产品名，不是谁的隐私。用 `has_ci(` 作为特征，避免整文件放行。
ALLOW_LINE_MARKERS = (
    "has_ci(",          # 设备型号识别表
)


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    return [f for f in out.decode().split("\0") if f]


def main():
    hits = []
    for name in tracked_files():
        if name in ALLOW_FILES:
            continue
        try:
            text = open(name, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue          # 二进制文件跳过

        lines = text.split("\n")
        for pat, label, _ in RULES:
            for m in re.finditer(pat, text):
                ln = text.count("\n", 0, m.start())
                # 行级豁免：该行含功能特征串就跳过
                if any(k in lines[ln] for k in ALLOW_LINE_MARKERS):
                    continue
                hits.append((name, ln + 1, label, m.group(0)))

    # 同一文件同一行同一类只报一次，避免刷屏
    seen, uniq = set(), []
    for h in hits:
        k = (h[0], h[1], h[2])
        if k in seen:
            continue
        seen.add(k)
        uniq.append(h)

    if uniq:
        print("发现 %d 处疑似真实信息：" % len(uniq))
        for name, line, label, frag in uniq:
            print("  %s:%d  [%s]  %s" % (name, line, label, frag))
        print("\n示例数据请用 192.168.1.x / 192.168.2.x 之类的约定地址，"
              "或 <客户端> / <本机 WAN 地址> 这类占位符。")
        return 1

    print("隐私检查通过：%d 个已跟踪文件无真实内网地址/主机名" % len(tracked_files()))
    return 0


if __name__ == "__main__":
    sys.exit(main())

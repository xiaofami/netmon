#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把线上真实数据脱敏成可以公开的演示数据。

为什么需要
----------
`www/preview-live.html` 是「内嵌真实 dashboard 数据 + 让 xhr 直接读本地」的
离线预览页，用来验证前端、做交付截图。但它包含真实内网拓扑、真实设备名
（家里的手机 / 摄像头 / NAS 型号）、真实域名与公网段，**不能提交到公开仓库**。

本脚本读取那种预览页，把所有可识别到个人的痕迹替换掉，输出
`www/demo.html`——同名同构、交互完全一致，只是数据变成了示例值。
演示页用来：GitHub Pages 在线演示 / README 截图 / 无头浏览器回归测试。

替换策略
--------
* 内网 IPv4：`192.168.1.7` → `192.168.1.7`（保留主机号，读起来自然）
                其它私网段按出现顺序分配到 192.168.2.x / 192.168.3.x
* 公网 IPv4：按 /24 映射到 198.18.0.0/15（RFC 2544 保留段），保证不同 /24
              映射后仍然不同——CIDR 榜单不会出现一堆相同前缀
* IPv6：按 /32 映射到 2001:db8::/32（RFC 3849 文档段）
* 域名：按语义分类（广告 / 视频 / CDN / 云存储 / 更新 / 推送 / 社交 / NTP /
        厂商 IoT / 默认），每类有一组**真实存在的公开域名**作为替换池，
        按 hash 稳定取一个。这样榜单看起来像正常家庭流量，又不会暴露
        「这台机器在访问哪家私有服务」
* 设备名：按设备类型生成 `iPhone-3` / `NAS-1` / `IPCam-2` 这类通用名，
          vendor 同步替换；MAC 统一成 02:00:00:xx:xx:xx（本地管理位）
* 公开 DNS（1.1.1.1 / 8.8.8.8 / 223.5.5.5 …）原样保留：它们不指向个人

用法
----
    python3 build/make-demo.py                      # 读 preview-live.html
    python3 build/make-demo.py --input x.html --out www/demo.html
    python3 build/make-demo.py --check              # 只扫描不写文件，列出残留
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ── 保留原样的公开 DNS ─────────────────────────────────────────────────────
PUBLIC_DNS = {
    "1.1.1.1", "1.0.0.1", "8.8.8.8", "8.8.4.4", "9.9.9.9",
    "114.114.114.114", "223.5.5.5", "223.6.6.6", "119.29.29.29",
    "180.76.76.76", "101.226.4.6",
}

# ── 域名替换池（全部为真实存在的公开服务域名，不含任何私有信息）──────────
POOLS = {
    "ad": [
        "doubleclick.net", "googleadservices.com", "adservice.google.com",
        "gdt.qq.com", "cpro.baidu.com", "tanx.com", "umeng.com",
        "app-measurement.com", "adjust.com", "appsflyer.com",
        "unityads.unity3d.com", "ad.doubleclick.net", "adsmind.gdtimg.com",
        "c3.gdt.qq.com", "qzs.gdtimg.com", "v2mi.gdt.qq.com",
        "adashx.m.taobao.com", "analytics.editcookie.com", "bugly.qq.com",
        "sentry.io", "applog.mi.com", "log.avlyun.com", "stats.bigo.tv",
        "adx.adsame.com", "tracking.miui.com", "metrics.icloud.com",
    ],
    "video": [
        "upos-sz-mirrorcos.bilivideo.com", "api.bilibili.com",
        "cache.video.iqiyi.com", "v.youku.com", "vd.l.qq.com",
        "vod.cloudfront.net", "nflxvideo.net", "occ-0-2851-3646.1.nflxso.net",
        "vod.300hu.com", "api.xsj.wasu.tv",
        "upos-hz-mirrorakam.akamaized.net", "video.goofish.com",
        "hls.douyinvod.com", "mgtv.com", "pcvideo.vip.qq.com",
    ],
    "cdn": [
        "cdn.jsdelivr.net", "fonts.gstatic.com", "ajax.googleapis.com",
        "img.alicdn.com", "static.cloudflareinsights.com",
        "unpkg.com", "cdnjs.cloudflare.com", "static.wikia.nocookie.net",
        "assets-cdn.github.com", "cdn.bootcdn.net", "lib.baomitu.com",
        "s1.hdslb.com", "i0.hdslb.com", "img14.360buyimg.com",
        "gw.alicdn.com", "p0.meituan.net",
    ],
    "cloud": [
        "obs.cn-north-4.myhuaweicloud.com", "s3.amazonaws.com",
        "global.azureedge.net", "storage.googleapis.com",
        "backup.icloud.com", "download.synology.com",
        "dbankcloud.cn", "nsp-hicloud-cloudbackupnorth4-p02-drcn.obs.dualstack.cn-north-4",
        "s3.us-west-2.amazonaws.com", "blob.core.windows.net",
        "drive.google.com", "pan.baidu.com", "api.nasvendor.com",
    ],
    "update": [
        "download.microsoft.com", "tlu.dl.delivery.mp.microsoft.com",
        "update.googleapis.com", "mirrors.ustc.edu.cn", "mirrors.tuna.tsinghua.edu.cn",
        "dl.google.com", "swcdn.apple.com", "download.codebuddy.cn",
        "autoupdate7.synology.com", "ota.huawei.com", "firmware.miwifi.com",
        "dl.delivery.mp.microsoft.com", "mirrors.aliyun.com", "repo.immortalwrt.org",
    ],
    "msg": [
        "gateway.icloud.com", "api.telegram.org", "mtalk.google.com",
        "msg.qq.com", "long.weixin.qq.com", "push.apple.com",
        "gateway.push.apple.com", "api-access.pangolin-sdk-toutiao.com",
        "wss.mqtt.iot.mi.com", "notify.nasvendor.com", "im.cloud.tencent.com",
        "a1.googlevideo.com", "msg.push.hicloud.com",
    ],
    "social": [
        "www.xiaohongshu.com", "api.zhihu.com", "www.douyin.com",
        "aweme.snssdk.com", "weibo.com", "www.taobao.com",
        "item.taobao.com", "api.github.com", "www.zhihu.com",
        "aggr5-normal-s8.amemv.com", "appcloud2.in.zhihu.com",
        "68687142.h5app.alipay.com", "apisnd.heytapmobi.com",
        "static.zhihu.com", "api.weibo.cn",
        "cc.10010.com", "api.translate.example.org",
    ],
    "ntp": [
        "0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org", "time.apple.com",
        "0.datadog.pool.ntp.org", "1.cn.pool.ntp.org", "time.windows.com",
        "ntp.aliyun.com", "pool.ntp.org",
    ],
    "vendor": [
        "api.miwifi.com", "iot.midea.com", "api.oppomobile.com",
        "heytapmobi.com", "device-api.vivo.com.cn", "ota.huawei.com",
        "api.smartlife.com", "iot.smartdevice.com", "api.homehub.io", "cloud.vendor-iot.com",
        "api.audiopro.com", "api.printvendor.com", "iot.isp-gateway.cn",
        "api.yeelight.com", "device.mi.com", "api.xiaomi.com",
    ],
    "default": [
        "www.google.com", "github.com", "www.apple.com", "www.microsoft.com",
        "www.cloudflare.com", "example.com", "www.wikipedia.org",
        "www.bing.com", "www.baidu.com", "www.amazon.com",
        "accounts.google.com", "a.nel.cloudflare.com", "api.github.com",
        "www.qq.com", "www.163.com", "www.sohu.com",
    ],
}

# 关键词 → 池。顺序敏感：先匹配特殊类别，最后落到 default。
KEYWORDS = [
    ("ad", ("gdt", "doubleclick", "adservice", "adx", "tanx", "umeng", "beacon",
            "analytics", "track", "cpro", "pangolin", "adashx", "applog", "log.",
            "measurement", "appsflyer", "adjust", "bugly", "crash", "sentry",
            "adsmind", "ugdtimg", "eventlog", "stats", "ad.", "-ad-")),
    ("video", ("rtmp", "vod", "video", "bilivideo", "iqiyi", "youku", "mgtv",
               "netflix", "nflx", "youtube", "googlevideo", "300hu", "wasu")),
    ("cdn", ("cdn", "img", "static", "jsdelivr", "gstatic", "alicdn", "unpkg",
             "cdnjs", "font")),
    ("cloud", ("obs.", "backup", "s3", "azure", "storage", "drive", "synology",
               "ugnas", "ugos", "dbankcloud", "hicloud")),
    ("update", ("update", "download", "dl.", "mirror", "ota.", "upgrade",
                "swcdn", "delivery.mp")),
    ("msg", ("push", "mtalk", "telegram", "weixin", "wx.qq", "msg.", "notify",
             "im.", "gateway.icloud")),
    ("social", ("xiaohongshu", "zhihu", "douyin", "snssdk", "amemv", "weibo",
                "taobao", "tmall", "jd.com", "github", "bilibili")),
    ("ntp", ("pool.ntp", "ntp.", "time.")),
    ("vendor", ("miwifi", "midea", "oppo", "heytap", "vivo", "huawei", "xiaomi",
                "mi.com", "roborock", "qingping", "phicomm", "ezviz", "ys7",
                "yamaha", "epson", "synology", "tplink", "tp-link")),
]

# ── 设备名生成 ─────────────────────────────────────────────────────────────
DEVICE_TEMPLATE = {
    "phone":  ("iPhone-{}",   "Apple"),
    "tablet": ("iPad-{}",     "Apple"),
    "pc":     ("ThinkPad-{}", "Lenovo"),
    "nas":    ("NAS-{}",      "Synology"),
    "router": ("Router-{}",   "TP-LINK"),
    "tv":     ("AndroidTV-{}", "Google"),
    "camera": ("IPCam-{}",    "Hikvision"),
    "audio":  ("Speaker-{}",  "Sonos"),
    "printer": ("Printer-{}", "HP"),
    "iot":    ("IoT-{}",      "Espressif"),
    "other":  ("Device-{}",   "Espressif"),
}

RE_DOMAIN = re.compile(r"\b((?:[A-Za-z0-9_-]+\.)+[A-Za-z]{2,})\b")
RE_IPV4 = re.compile(r"\b(\d{1,3}(?:\.\d{1,3}){3})(/\d{1,2})?\b")
RE_IPV6 = re.compile(r"\b([0-9a-fA-F]{0,4}(?::[0-9a-fA-F]{0,4}){2,7})(/\d{1,3})?\b")
RE_MAC = re.compile(r"\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b")


def stable_hash(text, mod):
    """稳定哈希：同一输入在不同机器/不同次运行都得到同一个索引。"""
    return int(hashlib.sha256(text.encode("utf-8")).hexdigest()[:8], 16) % mod


def pick_pool(domain):
    low = domain.lower()
    for name, keys in KEYWORDS:
        if any(k in low for k in keys):
            return name
    return "default"


class Anonymizer:
    """两遍处理：先扫全量 token 建映射，再一次性替换，保证一致性。"""

    def __init__(self):
        self.domain_map = {}
        self.ip_map = {}
        self.mac_map = {}
        self.device_map = {}
        self._pub24 = {}          # 公网 /24 前缀 → 保留段编号
        self._priv = {}           # 私网段前缀 → 替换前缀
        self._priv_hits = {}      # 私网段出现次数，用来决定谁是 192.168.1
        self._v6 = {}
        self._dev_seq = {}
        self._mac_seq = 0
        self._dom_used = set()    # 已分配的目标域名，保证不重复

    def finalize(self):
        """第一遍扫描结束后调用：让出现最频繁的私网段拿到 192.168.1。

        家里 LAN 段的设备条目远多于上游光猫段，按频次排序能让演示数据
        的网段编号符合直觉（LAN = 192.168.1.x）。
        """
        order = sorted(self._priv_hits.items(), key=lambda kv: -kv[1])
        self._priv = {k: "192.168.%d" % (i + 1) for i, (k, _) in enumerate(order)}

    # ── IP ────────────────────────────────────────────────────────────────
    def _map_ip(self, ip, suffix=""):
        if ip in PUBLIC_DNS:
            return ip + (suffix or "")
        if ip in self.ip_map:
            return self.ip_map[ip] + (suffix or "")

        parts = [int(x) for x in ip.split(".")]
        a, b = parts[0], parts[1]
        priv = (a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168))
        if priv:
            # 每个不同私网 /16 分到不同的 192.168.x
            key = "%d.%d" % (a, b)
            self._priv_hits[key] = self._priv_hits.get(key, 0) + 1
            if key not in self._priv:
                self._priv[key] = "192.168.%d" % (len(self._priv) + 1)
            new = "%s.%d" % (self._priv[key], parts[3])
        else:
            # 公网：同一个 /24 映射到同一个 198.18/198.19 网段
            key24 = "%d.%d.%d" % (a, b, parts[2])
            if key24 not in self._pub24:
                n = len(self._pub24)
                self._pub24[key24] = "198.%d.%d" % (18 + n // 256, n % 256)
            new = "%s.%d" % (self._pub24[key24], parts[3])

        self.ip_map[ip] = new
        return new + (suffix or "")

    def _map_ipv6(self, addr, suffix=""):
        if addr in self._v6:
            return self._v6[addr] + (suffix or "")
        # 只保留前两组做区分：2001:db8:NNNN::/48
        head = addr.split("::")[0].split(":")
        idx = len(self._v6) + 1
        new = "2001:db8:%x::" % (0x1000 + idx)
        if head and head[0]:
            pass
        self._v6[addr] = new
        return new + (suffix or "")

    # ── 域名 ──────────────────────────────────────────────────────────────
    def _map_domain(self, dom):
        low = dom.lower()
        # 本地域后缀（嗅探器会给内网域名加 .lan）单独处理，保持可读
        lan = low.endswith(".lan")
        base = low[:-4] if lan else low
        if base in self.domain_map:
            return self.domain_map[base] + (".lan" if lan else "")

        pool = POOLS[pick_pool(base)]
        # 用原域名的 hash 定起点，然后**线性探测**第一个没被占用的名字。
        # 必须保证「一个原域名对一个目标域名」：否则榜单里会出现两三行完全相同的
        # example.com，一眼就是假数据。池子够大时探测一两次就能命中，不需要造
        # 「example3.com」这种明显编出来的名字。
        start = stable_hash(base, len(pool))
        chosen = None
        for k in range(len(pool)):
            cand = pool[(start + k) % len(pool)]
            if cand not in self._dom_used:
                chosen = cand
                break
        if chosen is None:
            # 整个池子都被占了（上千个域名才会发生）：退化成加序号
            head, _, tail = pool[start].partition(".")
            n = 2
            chosen = "%s%d.%s" % (head, n, tail)
            while chosen in self._dom_used:
                n += 1
                chosen = "%s%d.%s" % (head, n, tail)
        self._dom_used.add(chosen)
        self.domain_map[base] = chosen
        return chosen + (".lan" if lan else "")

    # ── 设备 ──────────────────────────────────────────────────────────────
    def map_device(self, dev):
        if not isinstance(dev, dict):
            return dev
        key = dev.get("display") or dev.get("name") or dev.get("ip") or ""
        kind = dev.get("kind") or "other"
        if key not in self.device_map:
            tpl, vendor = DEVICE_TEMPLATE.get(kind, DEVICE_TEMPLATE["other"])
            self._dev_seq[kind] = self._dev_seq.get(kind, 0) + 1
            self.device_map[key] = (tpl.format(self._dev_seq[kind]), vendor)
        name, vendor = self.device_map[key]
        dev["name"] = name
        dev["display"] = name
        dev["vendor"] = vendor
        if dev.get("ip"):
            dev["ip"] = self._map_ip(dev["ip"])
        if dev.get("active_ip"):
            dev["active_ip"] = self._map_ip(dev["active_ip"])
        if dev.get("mac"):
            dev["mac"] = self._map_mac(dev["mac"])
        for f in ("aliases", "peers"):
            if isinstance(dev.get(f), str) and dev[f]:
                dev[f] = self._map_text(dev[f])
        return dev

    def _map_mac(self, mac):
        if mac in self.mac_map:
            return self.mac_map[mac]
        self._mac_seq += 1
        n = self._mac_seq
        # 02: 前缀 = 本地管理地址，不会与任何真实厂商 OUI 冲突
        new = "02:00:00:%02X:%02X:%02X" % (n // 65536, (n // 256) % 256, n % 256)
        self.mac_map[mac] = new
        return new

    # ── 文本兜底 ──────────────────────────────────────────────────────────
    def _map_text(self, s):
        # 设备名可能散落在 dns_recent / devices 子表里（没有 kind 字段，走不到
        # map_device），所以先按「长名优先」做一次精确整体替换。
        if self.device_map:
            for old in sorted(self.device_map, key=len, reverse=True):
                if old and old in s:
                    s = s.replace(old, self.device_map[old][0])
        s = RE_MAC.sub(lambda m: self._map_mac(m.group(1)), s)
        s = RE_IPV4.sub(lambda m: self._map_ip(m.group(1), m.group(2) or ""), s)
        s = RE_DOMAIN.sub(lambda m: self._map_domain(m.group(1)), s)
        return s

    def build_device_maps(self, data):
        """把 devices[] 里的每台设备登记好，供后面的精确替换使用。"""
        def visit(o):
            if isinstance(o, dict):
                if "kind" in o and ("display" in o or "name" in o):
                    key = o.get("display") or o.get("name") or ""
                    kind = o.get("kind") or "other"
                    if key and key not in self.device_map:
                        tpl, vendor = DEVICE_TEMPLATE.get(kind, DEVICE_TEMPLATE["other"])
                        self._dev_seq[kind] = self._dev_seq.get(kind, 0) + 1
                        self.device_map[key] = (tpl.format(self._dev_seq[kind]), vendor)
                    for alt in (o.get("name"), o.get("display")):
                        if alt and alt not in self.device_map:
                            self.device_map[alt] = self.device_map.get(key, ("Device-0", ""))
                for v in o.values():
                    visit(v)
            elif isinstance(o, list):
                for v in o:
                    visit(v)
        visit(data)

    def scan(self, s):
        """第一遍：只登记 token，不修改。"""
        if not isinstance(s, str):
            return
        for m in RE_MAC.finditer(s):
            self._map_mac(m.group(1))
        for m in RE_IPV4.finditer(s):
            self._map_ip(m.group(1))
        for m in RE_DOMAIN.finditer(s):
            self._map_domain(m.group(1))

    def walk(self, obj, scan_only=False):
        if isinstance(obj, dict):
            # device 结构走专门逻辑，保证 name/vendor/kind 一起换
            if "kind" in obj and ("display" in obj or "name" in obj):
                if scan_only:
                    self._map_ip(obj.get("ip") or "0.0.0.0")
                    if obj.get("mac"):
                        self._map_mac(obj["mac"])
                    key = obj.get("display") or obj.get("name") or ""
                    if key and key not in self.device_map:
                        tpl, vendor = DEVICE_TEMPLATE.get(
                            obj.get("kind") or "other", DEVICE_TEMPLATE["other"])
                        self._dev_seq[obj.get("kind") or "other"] = \
                            self._dev_seq.get(obj.get("kind") or "other", 0) + 1
                        self.device_map[key] = (
                            tpl.format(self._dev_seq[obj.get("kind") or "other"]), vendor)
                    return
                return self.map_device(obj)
            # 字典的 key 也可能是敏感值：clients 就是以「设备 IP」为 key 的。
            # 这里同时处理 key，否则替换完 value 还会漏掉 key。
            if scan_only:
                for k, v in obj.items():
                    if isinstance(k, str):
                        self.scan(k)
                    if isinstance(v, (dict, list)):
                        self.walk(v, scan_only)
                    elif isinstance(v, str):
                        self.scan(v)
                return
            rebuilt = {}
            for k, v in obj.items():
                nk = self._map_text(k) if isinstance(k, str) else k
                if isinstance(v, (dict, list)):
                    self.walk(v, scan_only)
                    rebuilt[nk] = v
                elif isinstance(v, str):
                    rebuilt[nk] = self._map_text(v)
                else:
                    rebuilt[nk] = v
            obj.clear()
            obj.update(rebuilt)
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                if isinstance(v, (dict, list)):
                    self.walk(v, scan_only)
                elif isinstance(v, str):
                    if scan_only:
                        self.scan(v)
                    else:
                        obj[i] = self._map_text(v)


def load_demo(path):
    """只从预览页里取数据块；HTML 模板一律用最新的 www/index.html。

    这样 demo.html 永远跟着前端源码走，不会因为预览页生成得早就带着旧版
    CSS/JS（也避免旧模板里的硬编码 IP 之类的东西被带进来）。
    """
    html = Path(path).read_text(encoding="utf-8")
    m = re.search(r"var NETMON_DEMO=(\{.*?\});</script>", html, re.S)
    if not m:
        raise SystemExit("找不到 NETMON_DEMO 数据块: %s" % path)
    return json.loads(m.group(1).replace("<\\/", "</"))


# 残留检测规则。私网地址是硬红线；主机名按「形态」而不是按具体型号检测 ——
# 默认固件给主机起什么名、消费设备爱用什么前缀，都是可枚举的模式，
# 比维护一份设备清单更耐漏，也不需要在源码里罗列任何真实设备。
RESIDUE_RULES = (
    (r"\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b",                 "私网 10/8"),
    (r"\b172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b",   "私网 172.16/12"),
    (r"\b192\.168\.(?!1\.|2\.)\d{1,3}\.\d{1,3}\b",         "非示例 192.168 段"),
    (r"\bDESKTOP-[A-Z0-9]{5,}\b",                                 "Windows 默认主机名"),
    (r"\b(?:OPPO|NOVA|MIX|GEN8|UGOS|WTA|ILOCZ)-?[A-Z0-9]{2,}\b", "疑似厂商型号"),
    (r"\b[A-Za-z][A-Za-z0-9]*-mibt[A-Z0-9]{4}\b",                 "小米设备后缀"),
)


def check(html):
    """检查还有没有可疑残留。命中即返回非零退出码，用来卡住发布。"""
    bad = []
    for pat, label in RESIDUE_RULES:
        for m in re.finditer(pat, html):
            bad.append("%s: %s" % (label, m.group(0)))
    return sorted(set(bad))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default=str(ROOT / "www" / "preview-live.html"))
    ap.add_argument("--out", default=str(ROOT / "www" / "demo.html"))
    ap.add_argument("--check", action="store_true", help="只检查输出里有无残留")
    args = ap.parse_args()

    if args.check:
        p = Path(args.out)
        if not p.exists():
            raise SystemExit("输出文件不存在: %s" % p)
        bad = check(p.read_text(encoding="utf-8"))
        print("残留 %d 处" % len(bad))
        for b in bad[:40]:
            print("  " + b)
        sys.exit(1 if bad else 0)

    src = Path(args.input)
    if not src.exists():
        raise SystemExit(
            "缺少输入文件: %s\n"
            "先用 build/make-preview.py 从路由器抓一份真实预览页，再脱敏。" % src)

    html = (ROOT / "www" / "index.html").read_text(encoding="utf-8")
    data = load_demo(src)
    an = Anonymizer()
    an.build_device_maps(data)        # 设备名要先登记，才能做全文精确替换
    an.walk(data, scan_only=True)     # 登记所有域名 / IP / MAC
    an.finalize()                     # 按频次决定私网段编号
    an.walk(data, scan_only=False)    # 统一替换

    blob = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    blob = blob.replace("</", "<\\/")

    # 再补一轮纯文本替换：JSON 里作为 *键* 出现的 IP（例如 clients 字典以
    # 「设备 IP」为 key）走不到上面的遍历逻辑，只有全文替换能兜住。
    pairs = {}
    for old, new in an.device_map.items():
        pairs[old] = new[0] if isinstance(new, tuple) else new
    pairs.update(an.ip_map)
    pairs.update(an.mac_map)
    pairs.update(an.domain_map)
    for old, new in sorted(pairs.items(), key=lambda kv: -len(kv[0])):
        if old and old != new:
            blob = blob.replace(old, new)

    marker = "<script>\n'use strict';"
    if marker not in html:
        raise SystemExit("index.html 里找不到 <script> 注入点")
    out_html = html.replace(marker, "<script>var NETMON_DEMO=" + blob + ";</script>\n" + marker, 1)

    out = Path(args.out)
    out.write_text(out_html, encoding="utf-8")

    bad = check(out_html)
    print("写出 %s (%d KB)" % (out, out.stat().st_size // 1024))
    print("  域名映射 %d 条 / IP 映射 %d 条 / 设备改名 %d 台 / MAC 重写 %d 个"
          % (len(an.domain_map), len(an.ip_map), len(an.device_map), len(an.mac_map)))
    print("  残留可疑项 %d 处" % len(bad))
    for b in bad[:20]:
        print("    " + b)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

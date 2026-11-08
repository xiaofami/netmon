#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""构建 netmon 的 x86_64 apk（单架构）

设计前提
--------
* 目标只有 x86_64（ImmortalWrt 25.12.2 / apk-tools 3.0.5 / musl）。
  二进制用目标机自带的 x86_64-openwrt-linux-musl gcc 编译，ABI 完全一致；
  也可以换成本脚本里注释掉的 SDK 交叉编译器。
* 包格式是 apk-tools 3 的原生 ADB v3（`ADBd`），用 SDK 自带的
  `staging_dir/host/bin/apk mkpkg` 生成，不依赖 buildroot make 体系。
* 运行时依赖全部来自官方 apk 仓库；sqlite3 以 amalgamation 静态链接，
  homeproxy/sing-box 是可选增强，不写进 depends。

用法
----
    python3 build/build-apk.py

产物：dist/netmon-1.0.0-<版本>.apk、dist/luci-app-netmon-1.0.0-<版本>.apk
"""
import argparse
import hashlib
import os
import re
import shutil
import pathlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "apkbuild"
OUT = ROOT / "dist"
SDK = ROOT / "build" / "sdk" / "immortalwrt-sdk-25.12.2-x86-64_gcc-14.3.0_musl.Linux-x86_64"
# apk-tools 3 的 mkpkg：来自 SDK 的 staging_dir/host（wrapper + loader + .apk.bin
# 必须保持同目录结构，不能只复制 wrapper）。SDK 的 build_dir / 原始压缩包可以删。
APK = SDK / "staging_dir" / "host" / "bin" / "apk"
SSH = ["ssh", "-F", str(Path.home() / "netmon-work" / "netmon.conf"), "r"]
VERSION = "1.0.0-r5"

# 目标架构：决定 apk 元数据里的 arch 字段与输出文件名。
# 由 --arch 覆盖。非 x86_64 的架构必须配合 --bin-dir（本地交叉编译产物），
# 因为默认路径是「从已装好的路由器上把二进制取回来」，那台机器只有一种架构。
ARCH = "x86_64"

# apk del 会连 /etc/config/netmon 一起 purge 掉，用户改的端口 / 保留期 /
# 存储位置就丢了。卸载前把配置存到数据库目录（那里有 netmon.db，不会被删），
# 重装时再恢复，形成「卸载保留设置」的闭环。
CONF_BACKUP = "/opt/netmon/data/.netmon.conf.bak"

POST_INSTALL = """#!/bin/sh
# 重装：恢复卸载前保存的用户配置，再应用 UCI、启用并启动服务。
if [ -f %(bak)s ]; then
    cp %(bak)s /etc/config/netmon
    rm -f %(bak)s
fi
# apk 检测到配置被改动时会把包内默认配置另存为 .apk-new。
# 实测这个文件是在 post-install **跑完之后**才落盘的，所以脚本里立刻删会扑空，
# 后台延迟删也会随脚本进程一起被回收。真正有效的位置是 /etc/init.d/netmon：
# 每次启动（含 netmon-apply 的重启）扫一次，装完第一次重启/下次开机即清理干净。
rm -f /etc/config/netmon.apk-new
if [ -x /etc/uci-defaults/99-netmon-uhttpd ]; then
    /etc/uci-defaults/99-netmon-uhttpd
    rm -f /etc/uci-defaults/99-netmon-uhttpd
fi
/etc/init.d/netmon enable >/dev/null 2>&1 || true
/usr/bin/netmon-apply >/dev/null 2>&1 || /etc/init.d/netmon restart >/dev/null 2>&1 || true
exit 0
""" % {"bak": CONF_BACKUP}

PRE_DEINSTALL = """#!/bin/sh
# 卸载前保存配置；先 disable 再 stop，避免 procd 立刻 respawn 把进程拉回来。
mkdir -p /opt/netmon/data
[ -f /etc/config/netmon ] && cp /etc/config/netmon %(bak)s
/etc/init.d/netmon disable >/dev/null 2>&1 || true
/etc/init.d/netmon stop >/dev/null 2>&1 || true
killall netmon >/dev/null 2>&1 || true
exit 0
""" % {"bak": CONF_BACKUP}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def fetch_binary(remote, dst, mode=0o755):
    """从目标机取回已编译好的 musl 二进制（本机开发时最省事）。"""
    with open(dst, "wb") as f:
        run(SSH + ["cat " + remote], stdout=f)
    os.chmod(dst, mode)
    assert dst.stat().st_size > 100000, "二进制异常小: %s" % dst
    print("  取回 %-22s %d bytes" % (dst.name, dst.stat().st_size))


def stage_binary(src, dst, mode=0o755):
    """使用本地已有的二进制（交叉编译产物 / --bin-dir 指定的目录）。

    CI 里没有路由器可连，走这条路：先由 SDK 工具链编出 netmon 与 netmon-report，
    再交给本脚本打包，保证包结构与本地构建完全一致（安装脚本、配置保留、
    apk-new 清理这些坑都是一次次踩出来的，不能在 CI 里另写一份）。
    """
    shutil.copy2(src, dst)
    os.chmod(dst, mode)
    assert dst.stat().st_size > 100000, "二进制异常小: %s" % src
    print("  使用 %-22s %d bytes  (%s)" % (dst.name, dst.stat().st_size, src))


def is_musl_elf(path):
    """粗查是不是目标机用的 ELF：防止误把本机 glibc 二进制打进包。"""
    head = pathlib.Path(path).read_bytes()
    if head[:4] != b"\x7fELF":
        return False, "不是 ELF"
    if head[4] != 2:
        return False, "不是 64 位 ELF"
    if b"ld-musl" not in head:
        return False, "没有 musl 解释器（可能是 glibc 构建）"
    return True, "musl ELF"


def build_root(pkg):
    root = WORK / pkg / "root"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    return root


def make_pkg(pkg, arch, root, infos, scripts, outname):
    cmd = [str(APK), "mkpkg", "-F", str(root),
           "-I", "name:" + pkg, "-I", "version:" + VERSION,
           "-I", "arch:" + arch]
    for k, v in infos:
        cmd += ["-I", "%s:%s" % (k, v)]
    for typ, path in scripts:
        cmd += ["-s", "%s:%s" % (typ, str(path))]
    out = OUT / outname
    cmd += ["-o", str(out)]
    run(cmd)
    print("  生成 %-38s %d bytes" % (out.name, out.stat().st_size))
    return out


def parse_args():
    ap = argparse.ArgumentParser(
        description="把 netmon 打成 apk（apk-tools 3 / ADB v3）")
    ap.add_argument("--bin-dir", metavar="DIR",
                    help="使用目录里已编译好的 netmon / netmon-report，"
                         "而不是从路由器取（CI 与交叉编译用）")
    ap.add_argument("--arch", default=ARCH,
                    help="目标架构，写入包元数据并拼进文件名（默认 %s）" % ARCH)
    ap.add_argument("--sdk", metavar="DIR", help="ImmortalWrt SDK 根目录")
    ap.add_argument("--version", default=VERSION, help="包版本（默认 %s）" % VERSION)
    ap.add_argument("--out", metavar="DIR", default=str(OUT), help="输出目录")
    return ap.parse_args()


def main():
    global APK, VERSION, OUT, ARCH

    args = parse_args()
    VERSION = args.version
    ARCH = args.arch
    OUT = Path(args.out)
    if args.sdk:
        APK = Path(args.sdk) / "staging_dir" / "host" / "bin" / "apk"

    assert APK.exists(), "找不到 SDK host apk: %s" % APK
    OUT.mkdir(parents=True, exist_ok=True)
    # 直接取源码目录，不用 package/netmon/files/ 的副本：
    # 双份副本必然漂移（曾出现包里 index.html 落后源码两个版本），
    # 打包与直连部署看到的前端必须完全一致。
    www = ROOT / "www"
    src = ROOT / "src"
    files = ROOT / "package" / "netmon" / "files"   # 仅 uci-defaults 仍从这里取

    print("=== 1. 准备目标架构二进制 ===")
    nb = WORK / "netmon.bin"
    rb = WORK / "report.bin"
    nb.parent.mkdir(parents=True, exist_ok=True)
    if args.bin_dir:
        bd = Path(args.bin_dir)
        stage_binary(bd / "netmon", nb)
        stage_binary(bd / "netmon-report", rb)
        for f in (nb, rb):
            ok, why = is_musl_elf(f)
            assert ok, "%s 不像目标机二进制：%s" % (f.name, why)
    else:
        fetch_binary("/tmp/netmon.pkg", nb)
        fetch_binary("/tmp/netmon-report.pkg", rb)

    print("=== 2. 组装 netmon 文件树 ===")
    # 防回归：前端版本戳必须来自源码，不允许出现包内副本漂移
    import re as _re
    # 版本戳形如「页面版本 2026-11-08d · ...」，结尾可能跟 "·" 再跟 "<"，
    # 早期正则写成 (\S+?)< 会匹配不到，这里只取版本号本身。
    stamp = _re.search(r"页面版本\s*([0-9A-Za-z._-]+)",
                       (www / "index.html").read_text(encoding="utf-8"))
    print("  前端版本戳: %s" % (stamp.group(1) if stamp else "未找到"))
    root = build_root("netmon")
    layout = [
        (nb, "usr/bin/netmon", 0o755),
        (rb, "usr/bin/netmon-report", 0o755),
        (src / "netmon-apply", "usr/bin/netmon-apply", 0o755),
        (src / "init.d-netmon", "etc/init.d/netmon", 0o755),
        (src / "config-netmon", "etc/config/netmon", 0o644),
        (files / "uhttpd-netmon", "etc/uci-defaults/99-netmon-uhttpd", 0o755),
        (www / "index.html", "www/netmon/index.html", 0o644),
        (www / "cgi-bin-data", "www/netmon/cgi-bin/data", 0o755),
        # LuCI 主实例（home=/www, cgi_prefix=/cgi-bin）用它自己的 CGI 路径，
        # 这样面板能在 LuCI 里同源嵌入，不需要额外端口。
        (www / "cgi-bin-data", "www/cgi-bin/netmon-data", 0o755),
    ]
    for src, rel, mode in layout:
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        os.chmod(dst, mode)
    (root / "opt" / "netmon" / "data").mkdir(parents=True, exist_ok=True)
    print("  %d 个文件 + 数据目录" % len(layout))

    print("=== 3. 生成安装/卸载脚本 ===")
    sp = WORK / "netmon" / "post-install"
    sd = WORK / "netmon" / "pre-deinstall"
    sp.write_text(POST_INSTALL, encoding="utf-8")
    sd.write_text(PRE_DEINSTALL, encoding="utf-8")
    for f in (sp, sd):
        os.chmod(f, 0o755)

    print("=== 4. 打包 netmon ===")
    # 注意：mkpkg 的 -I 对数组字段要一次给一个「空格分隔的字符串」。
    # 重复 -I depends:X 只会保留最后一个，依赖会被悄悄丢掉。
    p1 = make_pkg(
        "netmon", ARCH, root,
        [("description", "Passive network monitor (conntrack + DNS sniffing)"),
         ("license", "Apache-2.0"),
         ("origin", "netmon"),
         ("depends", "libc ip-full uhttpd kmod-nf-conntrack")],
        [("post-install", sp), ("pre-deinstall", sd)],
        "netmon-%s-%s.apk" % (VERSION, ARCH))

    print("=== 5. 组装并打包 luci-app-netmon ===")
    lroot = build_root("luci")
    luasrc = ROOT / "package" / "luci-app-netmon" / "luasrc"
    # 只有 controller + 设置页。1.0.0-r2 起去掉了 view/netmon/index.htm：
    # iframe 内嵌面板受 LuCI 主题尺寸限制，观感差，改为链接直达 /netmon/。
    luci_files = ("controller/netmon.lua",
                  "model/cbi/netmon/settings.lua")
    for rel in luci_files:
        dst = lroot / "usr/lib/lua/luci" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(luasrc / rel, dst)
        os.chmod(dst, 0o644)
    print("  %d 个 LuCI 文件" % len(luci_files))
    p2 = make_pkg(
        "luci-app-netmon", "noarch", lroot,
        [("description", "LuCI menu and settings page for netmon"),
         ("license", "Apache-2.0"),
         ("origin", "luci-app-netmon"),
         ("depends", "luci-base luci-compat netmon"),
         ("provides", "luci-app-netmon-any")],
        [],
        "luci-app-netmon-%s.apk" % VERSION)

    print("=== 6. 校验 ===")
    expect_deps = {p1.name: 4, p2.name: 3}
    for p in (p1, p2):
        head = p.read_bytes()[:4]
        assert head == b"ADBd", "%s 不是 ADB v3 包" % p
        dump = subprocess.run([str(APK), "adbdump", str(p)],
                              capture_output=True, text=True, check=True).stdout
        for key in ("name:", "version:", "depends:", "arch:"):
            assert key in dump, "%s 缺少 %s" % (p.name, key)
        m = re.search(r"depends: # (\d+) items", dump)
        n = int(m.group(1)) if m else 0
        assert n == expect_deps[p.name], \
            "%s 依赖数 %d，应为 %d（mkpkg 数组字段必须一次传空格分隔串）" % (
                p.name, n, expect_deps[p.name])
        assert "scripts:" in dump if p is p1 else True, "netmon 缺安装脚本"
        sha = hashlib.sha256(p.read_bytes()).hexdigest()[:16]
        print("  %-38s ADBd ✓  依赖 %d 项  sha256:%s" % (p.name, n, sha))
    print("完成：%s" % OUT)


if __name__ == "__main__":
    main()

# netmon - OpenWrt / ImmortalWrt 网络监控
#
# 常用目标：
#   make            编译 netmon 与 netmon-report
#   make test       跑单元测试（纯 C，不需要路由器）
#   make install    安装到 /usr/bin 与 /etc（在路由器上执行）
#   make clean
#
# SQLite 依赖
# -----------
# 采集与报表都直连 SQLite。默认使用系统 libsqlite3（有 pkg-config 就用它），
# 本机跑测试、调前端都够用：
#
#   make
#
# 交叉编译到路由器时用**静态库**，避免目标机缺 libsqlite3-0：
#
#   make SQLITE_CFLAGS=-I/path/to/sqlite-amalgamation \
#        SQLITE_LIBS=/path/to/sqlite-amalgamation/libsqlite3.a
#
# 也可以直接跑 src/get-sqlite.sh 自动下载 amalgamation 并编译静态库。

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -s
# 各 .c 文件自己在首行 #define _GNU_SOURCE（要在任何头文件之前），
# 命令行不要再加，否则会重复定义告警。
CFLAGS  += $(SQLITE_CFLAGS)

SQLITE_CFLAGS ?= $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   ?= $(shell pkg-config --libs sqlite3 2>/dev/null || echo -lsqlite3)

PREFIX  ?= /opt/netmon

# 已停用模块（源码保留在 src/deprecated/，不参与构建）：
#   probe.*  线路质量主动探测
#   reach.*  代理可达性探测
#   apps.*   服务类别推测 / 遥测域名识别
NETMON_SRC  = netmon.c db.c ctsample.c ctparse.c dnsmap.c dnssniff.c hprules.c
# 报表与采集共用同一份规则模块：两边读到的 homeproxy 规则、归因结果完全一致
REPORT_SRC  = netmon-report.c hprules.c detail.c

all: netmon netmon-report

netmon: $(NETMON_SRC) $(wildcard *.h)
	$(CC) $(CFLAGS) -o $@ $(NETMON_SRC) $(SQLITE_LIBS)

netmon-report: $(REPORT_SRC) $(wildcard *.h)
	$(CC) $(CFLAGS) -o $@ $(REPORT_SRC) $(SQLITE_LIBS)

# 注意：ImmortalWrt 的 busybox 没有 `install` 命令，只能用 mkdir + cp + chmod。
# 之前用 install 会静默失败（make 报错但文件没换），排查时极易误判"改动没生效"。
install: all
	mkdir -p $(PREFIX)/data
	cp netmon        /usr/bin/netmon        && chmod 755 /usr/bin/netmon
	cp netmon-report /usr/bin/netmon-report && chmod 755 /usr/bin/netmon-report
	cp netmon-apply  /usr/bin/netmon-apply  && chmod 755 /usr/bin/netmon-apply
	cp init.d-netmon /etc/init.d/netmon     && chmod 755 /etc/init.d/netmon
	@echo "服务脚本已经就位；UCI 配置由 apk 包安装到 /etc/config/netmon，"
	@echo "手工安装时请自行创建，或直接用 package/netmon 打包。"

test: test_ctparse test_dnsmap test_dnssniff
	./test_ctparse
	./test_dnsmap
	./test_dnssniff
	@echo "单元测试全部通过"

test_ctparse: test_ctparse.c ctparse.c ctparse.h
	$(CC) $(CFLAGS) -o $@ test_ctparse.c ctparse.c

# test_db 需要 SQLite：把 db.c / ctsample.c 一起编进来，跑真实建表与插入。
test_db: test_db.c db.c db.h ctsample.c ctparse.c
	$(CC) $(CFLAGS) -o $@ test_db.c db.c ctsample.c ctparse.c $(SQLITE_LIBS)

test_dnsmap: test_dnsmap.c dnsmap.c dnsmap.h
	$(CC) $(CFLAGS) -o $@ test_dnsmap.c dnsmap.c

# dnssniff 的测试用 `#define static` 把 .c 包进来，所以不单独链接 dnssniff.c
test_dnssniff: test_dnssniff.c dnssniff.c dnssniff.h
	$(CC) $(CFLAGS) -o $@ test_dnssniff.c

clean:
	rm -f netmon netmon-report netmon.new *.o
	rm -f test_ctparse test_db test_dnsmap test_dnssniff

.PHONY: all install test clean

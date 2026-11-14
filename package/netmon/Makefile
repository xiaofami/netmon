# netmon 基础包：采集进程 + 报表二进制 + 服务与页面
#
# 源码直接引用仓库根目录的 src/ 与 www/，**包内不保留副本**。
# 早期 ./files/src 与 ./files/index.html 各存了一份拷贝，结果出现
# 「包里前端落后源码两个版本」的漂移事故（docs/PACKAGING.md 有完整记录）。
# 单源之后这类问题从结构上消失。
#
# 编译依赖 SQLite 静态库。仓库不带二进制 .a，构建前先跑一次
#   sh src/get-sqlite.sh
# 想改用官方仓库的动态库：DEPENDS 加 +libsqlite3，把 $(SQLITE_A) 换成 -lsqlite3。

include $(TOPDIR)/rules.mk

PKG_NAME:=netmon
PKG_VERSION:=1.1.0
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

# 静态库位置。默认指向仓库根目录 build/sqlite/libsqlite3.a（get-sqlite.sh 生成）。
SQLITE_DIR ?= $(CURDIR)/../../build/sqlite
SQLITE_A   := $(SQLITE_DIR)/libsqlite3.a

SRC := $(CURDIR)/../../src
WWW := $(CURDIR)/../../www

define Package/netmon
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Passive network monitor (conntrack + DNS sniffing)
  # homeproxy / sing-box 是可选依赖，故意不写进 DEPENDS：
  # 没装时拦截归因自动降级，采集与其它功能完全可用。
  # SQLite 静态链接，运行时不需要 libsqlite3-0；
  # 主动探测已停用，也不再需要 curl。其余依赖全部来自官方仓库。
  DEPENDS:=+ip-full +uhttpd +kmod-nf-conntrack
endef

define Package/netmon/description
  只读 /proc/net/nf_conntrack 与 AF_PACKET udp/53 旁路嗅探，
  不参与转发、不发探测包、不改防火墙。提供设备画像、DNS 日志、
  流量排行与系统状态。
endef

define Build/Prepare
	test -f $(SQLITE_A) || \
	  (echo "缺少 $(SQLITE_A)，请先执行: sh src/get-sqlite.sh"; false)
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(SRC) $(PKG_BUILD_DIR)/src
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -I$(PKG_BUILD_DIR)/src \
	  -o $(PKG_BUILD_DIR)/netmon \
	  $(PKG_BUILD_DIR)/src/netmon.c $(PKG_BUILD_DIR)/src/db.c \
	  $(PKG_BUILD_DIR)/src/ctsample.c $(PKG_BUILD_DIR)/src/ctparse.c \
	  $(PKG_BUILD_DIR)/src/dnsmap.c $(PKG_BUILD_DIR)/src/dnssniff.c \
	  $(PKG_BUILD_DIR)/src/hprules.c $(SQLITE_A) -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -I$(PKG_BUILD_DIR)/src \
	  -o $(PKG_BUILD_DIR)/netmon-report \
	  $(PKG_BUILD_DIR)/src/netmon-report.c $(PKG_BUILD_DIR)/src/hprules.c \
	  $(PKG_BUILD_DIR)/src/detail.c $(SQLITE_A) -lm
endef

define Package/netmon/conffiles
/etc/config/netmon
endef

define Package/netmon/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config \
	  $(1)/opt/netmon/data $(1)/www/netmon/cgi-bin $(1)/www/cgi-bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/netmon $(1)/usr/bin/netmon
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/netmon-report $(1)/usr/bin/netmon-report
	$(INSTALL_BIN) $(SRC)/init.d-netmon $(1)/etc/init.d/netmon
	$(INSTALL_CONF) $(SRC)/config-netmon $(1)/etc/config/netmon
	$(INSTALL_DATA) $(WWW)/index.html $(1)/www/netmon/index.html
	$(INSTALL_BIN) $(WWW)/cgi-bin-data $(1)/www/netmon/cgi-bin/data
	# LuCI 主实例（home=/www, cgi_prefix=/cgi-bin）用它自己的 CGI 路径，
	# 这样面板能同源访问，不需要额外端口。
	$(INSTALL_BIN) $(WWW)/cgi-bin-data $(1)/www/cgi-bin/netmon-data
	$(INSTALL_BIN) $(SRC)/netmon-apply $(1)/usr/bin/netmon-apply
	$(INSTALL_BIN) $(CURDIR)/files/uhttpd-netmon $(1)/etc/uci-defaults/99-netmon-uhttpd
endef

define Package/netmon/postinst
#!/bin/sh
if [ -x /etc/uci-defaults/99-netmon-uhttpd ]; then
    /etc/uci-defaults/99-netmon-uhttpd
    rm -f /etc/uci-defaults/99-netmon-uhttpd
fi
/etc/init.d/netmon enable >/dev/null 2>&1 || true
/usr/bin/netmon-apply >/dev/null 2>&1 || /etc/init.d/netmon restart >/dev/null 2>&1 || true
exit 0
endef

define Package/netmon/prerm
#!/bin/sh
/etc/init.d/netmon stop >/dev/null 2>&1 || true
/etc/init.d/netmon disable >/dev/null 2>&1 || true
exit 0
endef

$(eval $(call BuildPackage,netmon))

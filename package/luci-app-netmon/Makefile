# LuCI 应用包：菜单 + CBI 设置页。
# 用普通 package.mk 而不是 luci.mk —— SDK 默认不含 luci feeds，
# 这样只依赖官方仓库里已有的 luci-base / luci-compat，构建更自包含。
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-netmon
PKG_VERSION:=1.1.0
PKG_RELEASE:=2
PKG_LICENSE:=Apache-2.0
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-netmon
  SECTION:=luci
  CATEGORY:=LuCI
  TITLE:=LuCI support for netmon
  DEPENDS:=+luci-base +luci-compat +netmon
endef

define Package/luci-app-netmon/description
  LuCI menu entry and settings page for the passive network monitor
  netmon. The dashboard itself is a standalone page served at /netmon/.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/luasrc
	$(CP) ./luasrc/* $(PKG_BUILD_DIR)/luasrc/
endef

define Package/luci-app-netmon/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller \
	  $(1)/usr/lib/lua/luci/model/cbi/netmon
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luasrc/controller/netmon.lua \
	  $(1)/usr/lib/lua/luci/controller/netmon.lua
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luasrc/model/cbi/netmon/settings.lua \
	  $(1)/usr/lib/lua/luci/model/cbi/netmon/settings.lua
	# 1.0.0-r2 起不再安装 /usr/lib/lua/luci/view/netmon/index.htm（iframe 面板已移除），
	# 升级时 apk 会自动删掉旧版本残留的该文件。
endef

$(eval $(call BuildPackage,luci-app-netmon))

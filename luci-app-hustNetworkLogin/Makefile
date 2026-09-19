include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-hustNetworkLogin
PKG_VERSION:=1.0.0
PKG_RELEASE:=5

LUCI_TITLE:=hustNetworkLogin
LUCI_DEPENDS:=+hustNetworkLogin
LUCI_PKGARCH:=all

# 绝对路径定位 luci.mk，无论包放在 package/ 还是 feeds/luci/applications/ 都可用
include $(TOPDIR)/feeds/luci/luci.mk

# 调用 BuildPackage 打包 LuCI 应用
$(eval $(call BuildPackage,luci-app-hustNetworkLogin))

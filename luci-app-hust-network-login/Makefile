include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-hust-network-login
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

LUCI_TITLE:=HUST Network Login 校园网自动登录
LUCI_DEPENDS:=+hust-network-login
LUCI_PKGARCH:=all

# 绝对路径定位 luci.mk，无论包放在 package/ 还是 feeds/luci/applications/ 都可用
include $(TOPDIR)/feeds/luci/luci.mk

# 调用 BuildPackage 打包 LuCI 应用
$(eval $(call BuildPackage,luci-app-hust-network-login))

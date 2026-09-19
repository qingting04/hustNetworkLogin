include $(TOPDIR)/rules.mk

PKG_NAME:=hustNetworkLogin
PKG_VERSION:=0.2.0
PKG_RELEASE:=3
PKG_BUILD_DEPENDS:=curl openssl libubox libubus

PKG_LICENSE:=MIT
PKG_MAINTAINER:=HUST campus network

# 纯 C 源码，OpenWrt 原生交叉编译（$(TARGET_CC)），menuconfig 选架构后 make 自动编译，
# 无需下载任何预编译二进制。
include $(INCLUDE_DIR)/package.mk

define Package/hustNetworkLogin
  SECTION:=net
  CATEGORY:=Network
  TITLE:=HUST campus network auto login (C)
  URL:=https://github.com/black-binary/hust-network-login
  DEPENDS:=+libcurl +libopenssl +libubus +libubox
endef

define Package/hustNetworkLogin/description
  Automatic Srun ePortal login for the HUST campus network (C implementation),
  reconnects automatically 15 seconds after the connection drops. Exposes the
  ubus object "hust-network-login" (status / reconnect) for the LuCI page and
  for scripts - no rpcd plugin needed.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -pthread \
		-I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/hust-network-login $(PKG_BUILD_DIR)/main.c \
		$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -lcurl -lcrypto -lubus -lubox -pthread
endef

define Package/hustNetworkLogin/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hust-network-login $(1)/usr/bin/hust-network-login
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/hust-network-login $(1)/etc/init.d/hust-network-login
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/hust-network-login $(1)/etc/config/hust-network-login
endef

$(eval $(call BuildPackage,hustNetworkLogin))

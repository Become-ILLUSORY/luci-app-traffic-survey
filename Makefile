# SPDX-License-Identifier: GPL-2.0-only
#
# Per-host traffic survey for OpenWrt/ImmortalWrt (MTK HNAT aware).
#
# Statistics source: nf_conntrack accounting (nf_conntrack_acct). On
# MediaTek targets with hardware flow offloading (mtkhnat), PPE per-entry
# counters are periodically merged back into conntrack by the kernel when
# 'hnat counter update to nf_conntrack' is enabled (hnat_setting 7 1), so
# the numbers stay accurate even with HW acceleration on.
#
# Copyright (C) 2026

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-traffic-survey
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Become-ILLUSORY

LUCI_TITLE:=LuCI per-host traffic survey
LUCI_DESCRIPTION:=Online host list with per-client upload/download rates and \
 total traffic, based on conntrack accounting (accurate with MTK HW offload).
LUCI_DEPENDS:=+luci-base +rpcd +kmod-nf-conntrack \
	+libubox +libubus +libmnl +libnetfilter-conntrack +libnfnetlink

PKG_BUILD_DEPENDS:=luci-base/host

include $(INCLUDE_DIR)/package.mk

TARGET_CFLAGS += -Wall -Wextra -Wno-unused-parameter -O2
TARGET_LDFLAGS += -lubox -lubus -lblobmsg_json -lmnl -lnetfilter_conntrack -lnfnetlink

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-o $(PKG_BUILD_DIR)/luci.client-rates \
		./src/client-rates.c \
		$(TARGET_LDFLAGS)
endef

define Package/$(PKG_NAME)/conffiles
/etc/config/traffic_survey
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./root/etc/init.d/traffic-survey $(1)/etc/init.d/traffic-survey
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./root/etc/uci-defaults/90-traffic-survey $(1)/etc/uci-defaults/90-traffic-survey
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./root/etc/config/traffic_survey $(1)/etc/config/traffic_survey
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/luci.client-rates $(1)/usr/libexec/rpcd/luci.client-rates
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-traffic-survey.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-traffic-survey.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/status/include
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/status/include/40_dhcp.js $(1)/www/luci-static/resources/view/status/include/40_dhcp.js
endef

define Package/$(PKG_NAME)/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/traffic-survey enable
	/etc/init.d/traffic-survey start >/dev/null 2>&1
	/etc/init.d/rpcd reload >/dev/null 2>&1
	rm -f /tmp/luci-indexcache /tmp/luci-modulecache/*
}
exit 0
endef

define Package/$(PKG_NAME)/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/traffic-survey stop
	/etc/init.d/traffic-survey disable
	# Restore stock online-hosts include if luci-mod-status shipped one
	rm -f /www/luci-static/resources/view/status/include/40_dhcp.js
	rm -f /tmp/luci-indexcache /tmp/luci-modulecache/*
}
exit 0
endef

$(eval $(call BuildPackage,$(PKG_NAME)))

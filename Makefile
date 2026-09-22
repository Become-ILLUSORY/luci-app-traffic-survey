# SPDX-License-Identifier: GPL-2.0-only
#
# luci-app-traffic-survey: per-host traffic survey for OpenWrt/ImmortalWrt.
#
# Statistics source: nf_conntrack accounting via ctnetlink (C rpcd plugin,
# ubus object 'luci.client-rates'). On MediaTek targets with hardware flow
# offload (mtkhnat), the init script enables 'hnat counter update to
# nf_conntrack' (hnat_setting 7 1) so PPE per-entry counters are merged back
# into conntrack and offloaded traffic stays accounted.
#
# Copyright (C) 2026 Become-ILLUSORY

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-traffic-survey
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Become-ILLUSORY

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-traffic-survey
	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=3. Applications
	TITLE:=LuCI per-host traffic survey (HNAT aware)
	DEPENDS:=+luci-base +rpcd +kmod-nf-conntrack \
		+libubox +libubus +libmnl +libnetfilter-conntrack +libnfnetlink
	PKGARCH:=all
endef

define Package/luci-app-traffic-survey/description
	Online-hosts overview panel with per-client upload/download rates and
	per-MAC total traffic (2s refresh). Uses conntrack accounting, accurate
	with MediaTek hardware NAT offload (PPE counter feedback).
endef

TARGET_CFLAGS += -Wall -Wextra -Wno-unused-parameter -O2

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-o $(PKG_BUILD_DIR)/luci.client-rates \
		./src/client-rates.c \
		$(TARGET_LDFLAGS) \
		-lubox -lubus -lblobmsg_json -lmnl -lnetfilter_conntrack -lnfnetlink -lpthread
endef

define Package/luci-app-traffic-survey/conffiles
/etc/config/traffic_survey
endef

define Package/luci-app-traffic-survey/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./root/etc/init.d/traffic-survey $(1)/etc/init.d/traffic-survey
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./root/etc/uci-defaults/90-traffic-survey $(1)/etc/uci-defaults/90-traffic-survey
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./root/etc/config/traffic_survey $(1)/etc/config/traffic_survey
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/luci.client-rates $(1)/usr/libexec/rpcd/luci.client-rates
	$(INSTALL_BIN) ./root/usr/libexec/rpcd/traffic-survey-meta $(1)/usr/libexec/rpcd/traffic-survey-meta
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-traffic-survey.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-traffic-survey.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/status/include
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/status/include/40_dhcp.js $(1)/www/luci-static/resources/view/status/include/40_dhcp.js
endef

define Package/luci-app-traffic-survey/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/traffic-survey enable
	/etc/init.d/traffic-survey start >/dev/null 2>&1
	/etc/init.d/rpcd reload >/dev/null 2>&1
	rm -f /tmp/luci-indexcache /tmp/luci-modulecache/* 2>/dev/null
}
exit 0
endef

define Package/luci-app-traffic-survey/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/traffic-survey stop 2>/dev/null
	/etc/init.d/traffic-survey disable 2>/dev/null
	rm -f /www/luci-static/resources/view/status/include/40_dhcp.js
	rm -f /tmp/luci-indexcache /tmp/luci-modulecache/* 2>/dev/null
}
exit 0
endef

$(eval $(call BuildPackage,luci-app-traffic-survey))

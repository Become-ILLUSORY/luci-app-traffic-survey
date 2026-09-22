# luci-app-traffic-survey

在线主机流量勘测 for OpenWrt / ImmortalWrt（MTK HNAT 感知）。

在 **状态 → 概览** 里替换默认的「在线主机」面板，新增每台主机的
**上传速率 / 下载速率 / 总流量** 三列，每 2 秒刷新。

## 原理

统计数据全部来自 **conntrack accounting**（`/proc/net/nf_conntrack` 里每条流的
`bytes=` 计数）。关键在于它对 **硬件 NAT 加速（MTK HNAT / flow offload）是准的**：

- 包被卸载到 PPE 后不再进 CPU，传统上 conntrack 计数会停更、统计失效；
- 但 MT7981/7986/7988（`mtk-hnat_v4/v5`）的 PPE 维护 per-entry 硬计数器，内核通过
  `hnat counter update to nf_conntrack`（`/sys/kernel/debug/hnat/hnat_setting` 第 7 项）
  定期把 PPE 计数回灌到 conntrack；
- 本包的 init 脚本在开机时自动执行 `echo 7 1 > .../hnat_setting` 开启回灌，
  于是硬件转发的流量照样被逐主机统计。

代价：数据面零开销（PPE 计数是转发的天然副产品），只有定时回读的微秒级 CPU。

## 组成

| 文件 | 作用 |
|---|---|
| `usr/libexec/rpcd/luci.client-rates` | rpcd exec 插件，ubus 对象 `luci.client-rates`（兼容 QWRT API）；内含采样守护 |
| `etc/init.d/traffic-survey` | 开机开 `nf_conntrack_acct` + HNAT 回灌，procd 拉起采样循环 |
| `htdocs/.../status/include/40_dhcp.js` | 概览页「在线主机」面板（上传/下载/总流量列，2s 轮询） |
| `usr/share/rpcd/acl.d/...json` | ACL 授权 |
| `po/zh_Hans/` | 中文翻译 |

## 编译（chasey-dev/immortalwrt-mt798x-rebase）

```sh
# 1) 把包放进 SDK/源码树
cp -r luci-app-traffic-survey package/emortal/   # 或任意 feed 目录

# 2) 选中
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig   # LuCI -> Applications -> luci-app-traffic-survey -> <M>

# 3) 编译
make package/luci-app-traffic-survey/compile V=s
# 产物: bin/packages/*/base/luci-app-traffic-survey_*_all.apk
```

`.apk` 是 `PKGARCH=all`，直接 `apk add --allow-untrusted` 到运行中的路由器即可，不必重刷固件。

## 安装后

```sh
apk add --allow-untrusted luci-app-traffic-survey_1.0.0-1_all.apk
# 浏览器: 状态 -> 概览 -> 在线主机  (强制刷新一次页面清 LuCI 缓存)
```

## 验证 HNAT 回灌是否生效

```sh
cat /sys/kernel/debug/hnat/hnat_setting   # 看 "nf_conntrack update" 状态
logread | grep traffic-survey             # 应有 "enabled HNAT counter feedback"
# 跑大流量时:
grep <某主机IP> /proc/net/nf_conntrack | head -1   # bytes= 应持续增长
ubus call luci.client-rates get '{"addresses":["192.168.0.10"]}'
```

## 与 QWRT 版的关系

QWRT 是 lean 的闭源固件，其统计后端 `rpcd/luci.so` 不开源。本包用纯 shell + awk
重新实现了等价的 `luci.client-rates` ubus API（协议字段逐一对齐），并复刻了概览页
前端。去掉了 QWRT 专属的 OUI 厂商库、指纹识别、Web 端口探测，保持零额外依赖。

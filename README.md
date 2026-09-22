# luci-app-traffic-survey

在线主机流量勘测 for OpenWrt / ImmortalWrt（MTK HNAT 感知）。

在 **状态 → 概览** 里替换默认的「在线主机」面板，新增每台主机的
**上传速率 / 下载速率 / 总流量** 三列，每 2 秒刷新。

## 最低要求

| 类别 | 要求 | 说明 |
|---|---|---|
| **固件基线** | OpenWrt / ImmortalWrt，内核 ≥ 5.4（推荐 6.x） | 依赖 netfilter conntrack accounting 与 ctnetlink |
| **包管理器** | APK（ImmortalWrt ≥ 25.x）或 OPKG（OpenWrt ≤ 24.x） | CI 按目标分支产出对应格式 |
| **Web 界面** | LuCI（JS 版，`luci-base`） | 前端是 LuCI `status/include` 面板 |
| **RPC 层** | `rpcd` + `ubusd` | 后端是 rpcd/ubus 插件 |
| **运行时依赖** | `libubox` `libubus` `libmnl` `libnetfilter-conntrack` `libnfnetlink` | 安装时自动拉取 |
| **内核配置** | `CONFIG_NF_CONNTRACK=y`、`CONFIG_NF_CT_ACCT`（或运行时可开 `nf_conntrack_acct`） | 缺了会显示"统计不可用" |
| **内存** | ≥ 128 MB | 守护进程常驻内存仅数百 KB |

## 适用条件（什么情况能用 / 能用但有限制 / 不能用）

### ✅ 完全可用（统计精确）

- **MediaTek MT7981 / MT7986 / MT7988**（filogic，`mtk-hnat_v4/v5`，`per_flow_accounting=true`），开了硬件 NAT 加速——PPE 硬计数器回灌，硬转流量照样逐主机统计。**这是本包的主打场景**，典型机型：CMCC RAX3000M-NAND 等。
- 任何**纯软转发 / 软 flow offload** 的 OpenWrt/ImmortalWrt（x86、MT7622、IPQ 等）——包全过 CPU，conntrack 计数天然完整。
- 完全**关闭**任何 offload 的设备。

### ⚠️ 能用但有限制

- **非联发科的硬件 offload**（如高通 NSS、博通 FA/CTF）：本包只实现了 MTK 的计数回灌。这些平台上开了硬转后，被卸载的流 conntrack 计数停更 → **速率显示偏低或 0，总量偏小**（和未感知 offloading 的传统统计一样的毛病）。关掉硬转即恢复精确。
- **MTK 但较老的 SoC**（`per_flow_accounting=false`，如部分 MT7621/7622 PPE）：无 per-entry 硬计数，硬转下统计不准；软转正常。
- **大流量冲击瞬间**：回灌有周期（秒级），速率比瞬时真实值略"钝"，但总量分毫不差。

### ❌ 不可用

- **非 LuCI 界面**（如纯 CLI、其它 Web 框架）——前端是 LuCI 面板。
- **内核未启用 conntrack**（极简容器/定制内核）。
- **无 rpcd/ubus** 的系统。

> 一句话：**联发科 MT798x + 开硬转 = 本包存在的意义**；其它软转平台也能用，但那是顺带兼容。

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
| `src/client-rates.c` → `usr/libexec/rpcd/luci.client-rates` | **C 后端**：常驻 ubus 守护，ubus 对象 `luci.client-rates`，libmnl + libnetfilter_conntrack 做 ctnetlink dump，uloop 定时 2s 采样，per-MAC 总量持久化到 `/tmp` |
| `etc/init.d/traffic-survey` | 开机开 `nf_conntrack_acct` + HNAT 回灌（`echo 7 1 > hnat_setting`），procd 拉起并守护 C 守护进程 |
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

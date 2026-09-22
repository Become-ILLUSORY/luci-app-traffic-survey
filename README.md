# luci-app-traffic-survey

在线主机流量勘测 for OpenWrt / ImmortalWrt（MTK HNAT 感知）。

在 **状态 → 概览** 里替换默认的「在线主机」面板，新增每台主机的
**上传速率 / 下载速率 / 总流量** 三列，每 2 秒刷新。

## 最低要求（不满足则无法使用）

这些是**硬性前置条件**，缺任何一条都会导致功能不可用或部分不可用：

| 类别 | 必须满足 | 不满足的后果 |
|---|---|---|
| **固件** | OpenWrt 或 ImmortalWrt | 非此系固件无 LuCI/rpcd/ubus，整套机制不成立 |
| **内核** | ≥ 5.4（推荐 6.x），`CONFIG_NF_CONNTRACK=y` | 无 conntrack → 统计完全不可用 |
| **conntrack 记账** | `CONFIG_NF_CT_ACCT=y`，或运行时可开 `net.netfilter.nf_conntrack_acct=1` | 无法开启 → 速率/总量恒为 0，前端显示"统计不可用" |
| **RPC/UBus** | `rpcd` 运行中 + `ubusd` 运行中 | 后端插件无法注册，前端拿不到数据 |
| **Web 界面** | LuCI（JS 版 `luci-base`） | 前端是 LuCI `status/include` 面板，非 LuCI 界面看不到 |
| **运行时依赖** | `libubox` `libubus` `libmnl` `libnetfilter-conntrack` `libnfnetlink` | 安装时自动拉取；被裁剪掉则后端无法启动 |
| **包管理器** | APK（ImmortalWrt ≥ 25.x）或 OPKG（OpenWrt ≤ 24.x） | 用对应格式的包安装 |
| **内存** | ≥ 128 MB | 守护常驻内存仅数百 KB，极低端设备需留意 |

> **验证最低要求**：`sysctl net.netfilter.nf_conntrack_acct`（应为 1）、`ubus list`（应有响应）、`test -c /dev/netlink 或内核支持 ctnetlink`。

## 功能与额外条件

三个增强功能各自有独立条件，**不影响核心流量统计**；不满足时对应功能静默降级：

| 功能 | 额外条件 | 不满足时 |
|---|---|---|
| **逐主机流量统计**（核心） | 见上表"最低要求"；MTK 硬转下需 HNAT 支持 per-flow accounting（见下节） | 速率为 `-`/0 |
| **厂商识别（Vendor）** | 能访问外网 `api.macvendors.com`（仅首次查询某 MAC 时需要，结果缓存到本地） | 该列显示 `-`，其余功能不受影响；随机 MAC 本就不查 |
| **设备类型** | dnsmasq 租约里带有可识别的 hostname / vendor-class | 显示 `-`（纯启发式，无指纹库，不保证识别率） |
| **Web 管理页直达** | 目标设备确实开放了受支持的 Web 端口（80/443/8080/8443/81/8000/5000/9000/5666/5667 之一） | IP 保持纯文本不可点；探测本身零开销、可忽略 |

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
| `src/client-rates.c` → `usr/libexec/rpcd/luci.client-rates` | **C 后端**：常驻 ubus 守护，ubus 对象 `luci.client-rates`；libmnl + libnetfilter_conntrack 做 ctnetlink dump，uloop 定时 2s 采样，per-MAC 总量持久化到 `/tmp`；内嵌 **Web 端口探测线程**（非阻塞扫常见管理端口，结果经 `web_port` 字段返回） |
| `root/usr/libexec/rpcd/traffic-survey-meta` | **设备元信息后端**（shell ubus 插件 `traffic-survey-meta`）：OUI 厂商查询（api.macvendors.com，结果缓存到 `/tmp/traffic-survey-vendor/`，随机 MAC 跳过）+ 设备类型启发式（解析 dnsmasq 租约的 hostname/vendor-class，不抓包） |
| `etc/init.d/traffic-survey` | 开机开 `nf_conntrack_acct` + HNAT 回灌（`echo 7 1 > hnat_setting`），procd 拉起并守护 C 守护进程 |
| `htdocs/.../status/include/40_dhcp.js` | 概览页「在线主机」面板：上传/下载/总流量列（2s 轮询）、**厂商列**（懒加载不阻塞）、**可点 IP**（直达设备 Web 后台） |
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

包内有 C 二进制（aarch64），是架构相关包；同名 SoC 家族的固件可直接
`apk add --allow-untrusted` 到运行中的路由器，不必重刷。

### 用 GitHub Actions 编译（推荐，已内置）

仓库自带 `.github/workflows/build.yml`：push 即触发，自动拉
ImmortalWrt 25.12.2 mediatek/filogic SDK（aarch64_cortex-a53）编译，
产物（.apk）上传到 Artifacts；打 tag 会同时挂到 Release。无需本地工具链。

## 安装后

```sh
apk add --allow-untrusted luci-app-traffic-survey-1.0.0-r1.apk
/etc/init.d/traffic-survey start
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

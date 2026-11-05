# R36S WiFi 频繁掉线：根因诊断（2026-11-04）

设备：`clone-r36s` / 192.168.1.53 / RK3326 / 内核 4.4.189 / Debian 13 trixie
现象：用户反馈"经常网卡 WiFi 掉线"，表现为 SSH 不通、需要重启或长时间等待才恢复。

## 结论：不是 WiFi 掉线，是 USB 总线把网卡整个摘掉

掉线发生在 **USB 层**，不是射频层（无 deauth、无信号劣化）。网卡从总线上消失后
wlan0 接口直接不存在，NetworkManager 连"已断开"都算不上，只能标记为设备被移除。
恢复靠 USB 重新枚举，而枚举本身经常失败（`-110` 超时），所以一掉就是几十分钟。

## 证据链（本次启动 20:36 起，2 小时内掉 3 次）

```
[ 3639.618] dwc2 ff300000.usb: NYET/NAK/ACK/other in non-error case, 0x00000002   ×15
[ 3639.618] usb usb1-port1: disabled by hub (EMI?), re-enabling...
[ 3639.618] usb 1-1: USB disconnect, device number 3
[ 3639.619] wpa_supplicant: wlan0: CTRL-EVENT-DISCONNECTED ... reason=3 locally_generated=1
[ 3639.619] NetworkManager: device (wlan0): activated -> unmanaged (reason 'unmanaged-link-not-init')
[ 3639.970] usb 1-1: new high-speed USB device number 4 using dwc2
[ 3645.138] usb 1-1: device descriptor read/64, error -110     ← 枚举超时
[ 3660.681] usb 1-1: new high-speed USB device number 5 using dwc2
[ 3665.849] usb 1-1: device descriptor read/64, error -110
   （—— 此后 55 分钟网卡不存在 ——）
[ 7018.363] usb 1-1: new high-speed USB device number 7 using dwc2   ← 22:33 才回来
[ 7142.569] usb usb1-port1: disabled by hub (EMI?), re-enabling...   ← 2 分钟后又掉
[ 7142.569] usb 1-1: USB disconnect, device number 7
[ 7143.103] usb 1-1: New USB device found, 0bda:8178                 ← 这次 0.3s 恢复
```

关键判读：

- `disabled by hub (EMI?)` —— 这是 usbcore 在端口状态寄存器报错后的标准输出，
  表示 **hub 层把端口禁用**，与 AP、信号、路由表无关。
- `reason=3 locally_generated=1` —— wpa_supplicant 的 reason 3 = DEAUTH_LEAVING，
  `locally_generated=1` 说明是本地驱动主动断开，不是路由器踢人。
- `error -110` = ETIMEDOUT，USB 控制传输超时；连读两次描述符都超时，说明
  480Mbps 链路在枚举阶段就不稳。

## 排除项（都排掉了）

| 怀疑点 | 实测 | 判定 |
| --- | --- | --- |
| 信号弱 | `/proc/net/wireless` 质量 100，-45 ~ -52 dBm | 排除 |
| 电池欠压 | 100%，4.195V，STATUS=Full | 排除 |
| USB 供电不足（over-current） | `dmesg | grep -ic over-current` = 0；无 `cannot enable` / `not accepting address` | 排除 |
| USB autosuspend | `1-1/power/control=on`，`level=on`（未启用自动挂起） | 排除 |
| 路由器踢人 | locally_generated=1，无 deauth 计数增长 | 排除 |
| 省电策略 | **`rtw_power_mgnt=1`、`rtw_ips_mode=1`（均开启）** | **命中** |
| 驱动版本 | 实际绑定 vendor `8192cu` **v4.0.2_9000.20130911（2013 年）** | **命中** |

## 根因

1. **驱动是 2013 年的 Realtek vendor 版 `8192cu.ko`**（`/sys/module/8192cu`，
   srcversion `DA2F80CCF7D8118F7974F8D`，version `v4.0.2_9000.20130911`）。
   系统里同时存在主线 rtlwifi 版 `rtl8192cu.ko`，但两者同名，先加载的 vendor 版
   占住注册，主线版报 `Error: Driver 'rtl8192cu' is already registered, aborting...`
   后加载失败（`lsmod` 中 `rtl_usb/rtlwifi` Used by 0）。
2. **省电全开**：`rtw_power_mgnt=1`（minimal power save）+ `rtw_ips_mode=1`
   （inactive power save）。IPS 让网卡空闲后休眠，靠 USB 唤醒；在 RK3326 的
   dwc2 hsotg 控制器上唤醒时序不稳，正好对上两次掉线的时机——
   21:36 那次紧跟前一秒的 ALSA underrun（ES 空闲），22:35 那次紧接我 SSH 重连（有流量唤醒）。
3. **dwc2 4.4 老控制器**：出现 `ChHltd set, but reason is unknown` 与成片
   `NYET/NAK/ACK/other in non-error case`，URB 状态机在高负载/唤醒瞬间出错，
   最终由 hub 层兜底禁端口。

即：**2013 年 vendor 驱动 + 省电唤醒 + 老 dwc2 控制器**三者叠加，USB 链路被打断，
且重新枚举的退避很长（本次 55 分钟），体验上就是"WiFi 经常掉、掉了很久回不来"。

## 可选修复（按风险从低到高）

### A. USB 复位看门狗（兜底，推荐先做）
每 20s 检查 wlan0 是否存在；连续 3 次不存在 → 对 dwc2 平台设备
`unbind`/`bind` 强制复位 USB 控制器，触发立刻重新枚举。

- 路径：`/usr/local/bin/wifi-watchdog.sh` + `wifi-watchdog.service`
- 效果：把"55 分钟恢复"压到 ≤ 60s
- 副作用：复位瞬间 USB 口上的其它设备（U 盘/手柄）也会重枚举
- 回退：`systemctl disable --now wifi-watchdog`

### B. 关掉驱动省电（低风险，大概率根治）
```
/etc/modprobe.d/8192cu.conf:
options 8192cu rtw_power_mgnt=0 rtw_ips_mode=0 rtw_enusbss=0
```
改完 `rmmod 8192cu; modprobe 8192cu`（会断网 5–10s）。远程操作失败的话重启即恢复
（配置只影响模块加载，不影响启动）。

### C. 换主线 rtlwifi 驱动（中风险，A+B 无效再试）
黑名单 vendor `8192cu`，强制加载 `rtl8192cu`。主线版 URB 行为不同，可能不再触发
端口禁用，但在 8188CU 上吞吐通常更低，也可能换一种不稳。

### D. 诊断基建：打开持久日志
现在 `/var/log/journal` 不存在 → journal 只在内存，重启即丢，历史掉线次数无法统计。
```
mkdir -p /var/log/journal && systemctl restart systemd-journald
# 或 /etc/systemd/journald.conf 设 Storage=persistent + SystemMaxUse=32M
```
之后可直接 `journalctl -b -1 -k | grep -i "disabled by hub"` 看历史。

## 复核方法

```bash
# 本次启动的掉线次数
sudo dmesg | grep -c "disabled by hub"
# 每掉一次会多一条 usb 1-1: USB disconnect
sudo dmesg | grep "USB disconnect"
# 当前链路质量与省电参数
cat /proc/net/wireless
for p in rtw_power_mgnt rtw_ips_mode rtw_enusbss; do
  printf "%s=" $p; cat /sys/module/8192cu/parameters/$p; done
```

## 已实施（2026-11-04 22:38，设备 192.168.1.53）

1. **watchdog 已上线并开机自启**
   - `/usr/local/bin/wifi-watchdog.sh`（755）
   - `/etc/systemd/system/wifi-watchdog.service`（enabled + active）
   - 日志 `/var/log/wifi-watchdog.log`，实测 5 分钟无 false positive（只有 `started` 一行）
   - 判定：wlan0 消失记 2 分、在但 ping 网关失败记 1 分，累计 ≥3（约 60s）即复位 dwc2；
     复位后冷却 120s，连续失败退避到 10 分钟
2. **省电参数已写入 `/etc/modprobe.d/8192cu.conf`**
   `options 8192cu rtw_power_mgnt=0 rtw_ips_mode=0 rtw_enusbss=0`
   —— 注意：**当前内核里跑的还是旧参数**（`rtw_power_mgnt=1`），要 `rmmod/modprobe`
   重载或下次重启才生效。重载会断网 10~20s，且 SSH 走的就是 wlan0，需后台脚本执行。
3. **journald 持久化已开**：`/etc/systemd/journald.conf.d/99-persist.conf`
   （`Storage=persistent` + `SystemMaxUse=32M`）。

   踩到的坑：这个镜像的 `/` 属主是 `ark`，`systemd-tmpfiles --create --prefix /var/log/journal`
   会报 `unsafe path transition / (owned by ark) → /var (owned by root)` 并**拒绝建目录**。
   手工建 `<machine-id>` 子目录 + `chown root:systemd-journal` + `chmod 2755`，
   再 `journalctl --flush` 才真正落盘（`/var/log/journal/<mid>/system.journal`）。
   副作用：ark 用户不再能直接读 system journal，要 `sudo journalctl`（或把 ark 加进
   `systemd-journal` 组）。

## 2026-11-04 23:19 更换网卡：内置 8192CU → 外置 RTL8188EU

用户换了外置 USB 网卡，MAC `20:f5:43:da:3e:f0`，DHCP 拿到 **192.168.1.81**（ssh 别名已同步）。

### 新网卡识别
- 芯片 RTL8188EU，驱动 vendor `8188eu.ko`（1,540,096 B），旧的 `8192cu` 仍加载但 `Used by 0`
- 默认参数：**`rtw_power_mgnt=2`（max power save）**、`rtw_ips_mode=1`
- dmesg 每 20 秒一轮 `Enter 802.11 power save - WIFI-TRAFFIC_IDLE` /
  `Leave ... WIFI-LPS_CTRL_LEAVE`，`rtl8188e_set_FwPwrMode_cmd: Mode=2`

### 后果与实测
| 状态 | 本机 → r36s | r36s → 网关 | 省电事件 |
| --- | --- | --- | --- |
| 换卡后默认 | avg 97ms（68–126） | — | 每 20s 一轮 |
| 关省电前（运行时写 sysfs） | — | avg 9.2ms | 仍在刷（写入不生效） |
| 重载后 `0/0` | **avg 13ms（min 3）** | **avg 9.6ms（min 4）** | **25s 内 0 次** |

### 关键发现：`/sys/module/*/parameters` 运行时写入是假生效
`rtw_power_mgnt` / `rtw_ips_mode` 文件权限是 `rw-r--r--`，root 能写、`cat` 也显示 0，
但驱动内部 PS 状态机不读这个变量，dmesg 照样 `Enter 802.11 power save`。
**必须 rmmod + modprobe 才能真生效**（8192cu 与 8188eu 同样如此）。

### 已实施
- `/etc/modprobe.d/8188eu.conf`：`options 8188eu rtw_power_mgnt=0 rtw_ips_mode=0`
- 用 `out/deploy/r36s/reload-8188eu.sh` 重载（root + 绝对路径 `/usr/sbin/rmmod|modprobe`）
- 重载耗时 25s（23:20:33 → 23:20:58），IP 保持 .81，watchdog 全程 active 未触发复位
- 旧网卡的 `/etc/modprobe.d/8192cu.conf` 保留（插回旧卡时仍然有用）

### 踩坑：这是 Debian 13 最小镜像，`rmmod`/`modprobe` 不在 sudo 的 PATH 里
`/sbin/rmmod` 是坏链（指向不存在的 `/bin/kmod`），可用的是 `/usr/sbin/rmmod`
（→ `/usr/bin/kmod`）。脚本里必须写绝对路径，否则 `command not found` 静默失败
——这也是第一次重载白跑一遍的原因。

### 仍待观察
8188EU 走的是同一个 dwc2 控制器，若仍出现 `disabled by hub (EMI?)`，
说明问题在控制器/供电侧而非网卡芯片，换卡不解决。判据同前：`dmesg` 计数 +
`/var/log/wifi-watchdog.log` 的 `resetting dwc2` 次数。

## 备注

- 这台设备的 `/tmp` 是 noexec，watchdog 脚本要放家目录或 `/usr/local/bin`。
- 重新枚举失败时（`-110`）系统会自己退避重试，不要反复 `modprobe`，直接复位 dwc2 更快。
- 同芯片在其它机型上未观察到该现象（8 台里只有 R36S 用 USB 8188CU 模块）。
- 复位 dwc2（`ff300000.usb` 的 unbind/bind）会让该 USB 口上的所有设备重枚举，
  插着 U 盘/手柄时会掉一次，属预期。

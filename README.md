# P4can-No-Reboot

**[English](README.en.md) · [Français](README.fr.md)**

**样本更新: 目前所有内测已确定解决重启，收到7个样本和视频**

**注意：需要自己手动在《更多》板块里打开开关**

> 修好一件小事：**开了「被封车辆 + TLSSC 恢复」之后，车机不用每轮上电都自动重启一次了。**

这是给 **Waveshare ESP32-P4-C6 WiFi6 3×TWAI**（32 MB flash）板子的特斯拉 CAN 工具固件成品包，
基于 [P4can-universal-unlock](https://github.com/06066060606060/P4can-universal-unlock) `v3.29.3`。
根目录是**编译好的固件**（`.bin`），完整源码在 [`source/`](source/) 子目录。

**界面默认中文**，右上角语言开关可切英文。

> [!TIP]
> **想先看界面、不刷机？** 仓库里附了一份**模拟页面**：
> [`source/sim/p4can-sim.html`](source/sim/p4can-sim.html) —— 下载到本地、双击用浏览器打开即可交互
> （GitHub 网页里直接点开只会看到源码，属正常现象）。
> 内置假后端、无需板子、不连真车，页面顶部有醒目的「模拟页面 / SIMULATION」标识。
> 可以放心点开体验所有开关和页面；它只演示界面，**不会改动任何真实车辆**。
> 真正刷机用的是下面根目录的 `.bin`。

> [!WARNING]
> **只适用于 32 MB Flash 的板子**（微雪 ESP32-P4-C6-WIFI6 / rev3.2）。
> **16 MB 版本刷这个包会直接变砖** —— 分区表 `app5M_fat24M_32MB` 与 16 MB flash 不匹配，
> 烧进去连 bootloader 都起不来，只能用 USB 重新救回来。源码原理都在，自己让AI适配。

---

## 最新版更新（v3.29.3-alight-shift-gpio-rebootfix）

最新成品发布在 [Releases 页面](https://github.com/baicyber/P4can-No-Reboot/releases/latest)，tag `v3.29.3-alight-shift-gpio-rebootfix`。仓库根目录保留的 `*-tlssc1-*.bin` 是上一个稳定回退版，供按需回退。

本版在 TLSSC 免重启之上新增：

- **灯光功能整页升级**：换用作者 v3.35.8 官方 lighting 模块（远光频闪 / 手动远光 / 后雾灯爆闪，拨杆手势触发），替换旧的灯光实现。
- **SmartShift 自动换挡**：从作者 v3.35.8 移植，默认关闭，设置页可开；带刹车确认。
- **CAN GPIO 改针脚**：设置页可改 CAN TX/RX 引脚（默认 20/21、22/23、26/27）；NVS 记录损坏时自动回退默认针脚，避免「刷完连不上」。
- **免重启再修复（rebootfix）**：修复休眠唤醒后车机重启与 P 挡随机重启。补发/echo 改为单次持锁读 epoch → 判 TX 屏障 → 发送，消除与每 5 ms 灯光任务的竞态；休眠唤醒后重新打开补发窗口；Model Y L 不补发未经确认的 0x331 路由。实车测试：不上电重启、不随机重启。
- **连接状态与灯光开关修复**：打开灯光等面板不再误报「未连接」；灯光开关可正常打开并保持。
- **开源项目地址**更新为 `P4can-No-Reboot`。

> [!NOTE]
> 本版含从作者内测源码移植的功能，**只发布成品 bin，不公开这部分源码**。TLSSC 免重启的最小修复仍见 [upstream PR #15](https://github.com/06066060606060/P4can-universal-unlock/pull/15)。

下载与校验见下方 [下载](#下载) 一节；刷完请对照页面顶部版本号确认。

---

## 它修复了什么

### 现象

「被封车辆 + TLSSC 恢复」开着时，**每轮上电都要自动重启一次车机**才有 TLSSC。

### 根因

TLSSC 恢复只做 `0x331` 的读-改-重发：**车发一帧，我们改一帧**。
而车机只在**自己启动时读一次** DAS 配置并锁存 —— 那一刻总线上要是还没有被改过的
`0x331`，车机就锁存「封禁」，得自动重启车机才重读。上游 Flipper 项目文档也把它直接标成
"Triggers MCU reboot"。

### 做法

把这个动作**提前到车机读配置之前**：

| 步骤 | 做法 |
|---|---|
| 缓存 | 把补丁后的 `0x331` 载荷存进 NVS（每个上电周期最多写一次，护 flash） |
| 补发 | 每次上电 CAN B 就绪后开 60 s 窗口，以 10 Hz 补发缓存帧，不等车先发 |
| 让位 | 车自己开始发 `0x331`（2 s 内有新鲜帧）就立刻停手，回到读-改-重发 |
| 收尾 | 窗口到期或补到 600 帧彻底停手，不会无限期造帧 |

只在「被封车辆」+「TLSSC 恢复」**都开着**时才动作；没有缓存帧（第一次用）就什么都不发；
走 CAN B，过与其它注入相同的 TX 屏障 / 管理员挂起 / USB 被动 / epoch 检查。

后续还修了一处：补发计数里偶发的「失败 1」不是帧发不出去，而是抢 TX 屏障锁时撞上
爆闪 / AP 注入返回 `ESP_ERR_TIMEOUT`，被误记成失败。现在单列为「让位」，不计失败、
下个周期自动重试；错误读数也从 `int8_t` 改 `int16_t`，不会再截断。

**v3.29.3-nr2 起，TLSSC 免重启开关默认开启**：首次刷入或从 nr1 升级后开关即为开；
不想用时到「更多功能 → TLSSC 免重启」手动关闭即可，之后会记住关闭状态。

nr3 另修一处：首页「Nag Killer · 提醒抑制」开关在旧版/损坏的 NAG 快照下点不动、
提示「Settings could not be saved」，以前只能整机 factory reset 恢复。现在允许用一份
校验过的新快照直接覆盖坏记录，写入成功后再清掉读失败锁存——**不用再重置**。
NVS 真不可写时仍会报错，不会掩盖真故障。

### 怎么验证

页面「更多功能 → TLSSC」开关下方的状态行分五种：关闭 / 缺前提 / 待首帧 / 车在自己发 / 补发 N 帧。
接口 `status.eapNr` 给可读字段：`enabled` / `prereq` / `cacheValid` / `cacheHex` /
`stockRx` / `replayOk` / `replayFail` / `burst` / `lastErr`。

**能保证什么：** 只要本机 CAN 比车机读配置先上线，车机读到的就是 `SELF_DRIVING` 的
`0x331`，不用自动重启；即使车机晚读，缓存帧也会在窗口内一直挂在总线上。

**不能保证什么：** 如果这辆车的 `0x331` 是周期性常发（本机会让位，改走读-改-重发），
而车机又在本机 CAN 上线之前就读完了配置，那一次仍然需要自动重启一次车机 ——
这是总线时序决定的，软件补不回来。以 `stockRx` / `replayOk` / `cacheHex` 三个读数判定。

### 顺带带来的能力

固件同时带：灯光功能整页（作者 v3.35.8）、更多功能页（滚轮换挡/动力狂暴/越野/
ESP 关闭/维护模式）、网页 OTA 升级、Wi-Fi 中继、抓包页、中英双语。这些是随基线一起
带出来的附带改动，不是本仓库的主要目的。

---

## 源码

完整源码在 [`source/`](source/) 子目录，与根目录的 `*-tlssc1-*.bin` 对应（v3.29.3 + TLSSC 免重启 + 灯光整页等改动）。

- 固件 sketch：`source/firmware/P4can-unlock-more/P4can-unlock-more.ino`
- 前端页面：`source/firmware/P4can-unlock-more/dashboard_source.html`
- 打包/构建脚本：`source/tools/more/`（`pack_ota.py`、`build_sim.py` 等）
- TLSSC 免重启实现：`source/firmware/P4can-unlock-more/feifan_eap_norestart.h`
- 变更历史：`source/CHANGELOG.md`

源码按上游同等方式开源，归属与授权见根目录 [`LICENSE`](LICENSE) 与 [`NOTICE`](NOTICE)。
其中灯光算法等取自作者 v3.35.6 参考实现，相关归属在源码注释与 NOTICE 中逐项标注。
本分支是功能集合分支；若只想看提交给上游的最小 TLSSC 免重启修复，见
[upstream PR #15](https://github.com/06066060606060/P4can-universal-unlock/pull/15)。

## 下载

**最新版（推荐）** 在 [Releases 页面](https://github.com/baicyber/P4can-No-Reboot/releases/latest) 下载：

| 文件 | 大小 | 用途 |
|---|---|---|
| `P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-ota.bin` | 1,521,920 B (1.45 MB) | **平时升级用**。只写 app 分区，失败可回滚 |
| `P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-full.bin` | 33,554,432 B (32 MB) | **首次烧录 / 救砖用**。整片镜像，含 bootloader 和分区表 |

校验值（最新版，已核对）：

| 文件 | sha256 |
|---|---|
| OTA | `51936e140681da03968bfacccaed070b80e7ffd7b780a712db8ea0f5811c0933` |
| full | `b19cb1226bc9c8be74f010600503f4253197b04ac3f626a78387794225f00165` |

**上一个稳定回退版**（仓库根目录，供需要回退时用）：

| 文件 | 大小 | sha256 |
|---|---|---|
| `P4can-unlock-more-v3.29.3-tlssc1-ota.bin` | 1,486,240 B | `4f6afb8309fdb25a6ff7b302d68924d000d9642b37b69ad331d43d81356fd35b` |
| `P4can-unlock-more-v3.29.3-tlssc1-full.bin` | 33,554,432 B | `bb61551f791192d6b1ef16312865e88e5f35bb786014e83dd25c2a074a34a462` |

下载后建议先校验：

```bash
sha256sum P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-ota.bin
# 应输出 51936e14...1c0933
```

---

## 刷机

### 升级包 `*-ota.bin` —— 平时更新用

1. 手机/电脑连上 P4 的热点
2. 打开 `http://192.168.4.1` → **设置 → 固件更新**
3. 选 `P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-ota.bin`
4. 点一下，再点一下确认，等进度条走完自动重启

OTA 只写 app 分区，**改不了 bootloader、分区表和 flash 参数**。

### 完整包 `*-full.bin` —— 首次烧录 / 救砖用

```bash
esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash 0x0 P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-full.bin
```

整片镜像布局（ESP32-P4 与 S3 不同，`0x0` 是空白 `0xFF`）：

| 偏移 | 内容 |
|---|---|
| `0x2000` | bootloader（magic `E9`） |
| `0x8000` | 分区表（magic `AA50`） |
| `0x10000` | 应用镜像（magic `E9`） |

刷完对照页面顶部版本号确认是 `v3.29.3-alight-shift-gpio-rebootfix`。

---

## 硬件与接线

板子：Waveshare ESP32-P4-WIFI6，rev3.2，32 MB flash，`app5M_fat24M_32MB` 分区。
`ota_0` / `ota_1` 各 `0x480000`（4.5 MB），单包上限 4,718,592 B。

| 总线 | TWAI | TX | RX |
|---|---|---|---|
| CAN 1 BODY | 0 | GPIO20 | GPIO21 |
| CAN 2 CHASSIS | 1 | GPIO22 | GPIO23 |
| CAN 3 PARTY | 2 | GPIO26 | GPIO27 |

---

## 已知限制

- `gauge` 仍是 503：0xB0 仪表、0x5D9 胎压载荷没解码。
- TLSSC 免重启依赖总线上电时序，边界见上文「不能保证什么」。
- 灯光频闪、后雾灯等帧定义来自静态逆向（证据等级 A），**没有实车抓包对照**；
  动态刹车灯、动力与制动域改动先在封闭场地确认。
- 分区表、bootloader、flash 参数只能线刷，OTA 无法更改。
- 没有在实车上逐项验证过所有写入行为。

---

## 安全与合规

这是给**封闭场地调试**用的工具。改动车身行为（灯光爆闪、免打扰抑制、
驾驶员监控抑制等）在公共道路上可能违法，也可能让车辆进入不符合型式认证的状态。

- 一切后果自负，作者不承担任何责任
- 不要在公共道路使用影响灯光、喇叭、限速识别的功能
- 刷机前确认你有办法救砖（完整包 + USB 线）

与特斯拉公司无任何关联。特斯拉、Model 3/Y 等商标归其所有者。
固件基线的版权与条款见 [NOTICE](NOTICE)，本项目新增部分的授权见 [LICENSE](LICENSE)。

---

## ☕ 赞助

> 固件免费。如果它帮你省下了折腾的时间，欢迎请作者喝杯咖啡，助力后续更新~

<table>
  <tr>
    <td align="center"><img src="docs/sponsor/web_alipay.jpg" width="220" alt="支付宝收款码"><br><sub><b>支付宝</b></sub></td>
    <td align="center"><img src="docs/sponsor/web_wechatpay.jpg" width="220" alt="微信收款码"><br><sub><b>微信支付</b></sub></td>
  </tr>
</table>

## 📮 联系作者

> 问题反馈，加好友注明来意。

<table>
  <tr>
    <td align="center"><img src="docs/sponsor/web_douyin.jpg" width="220" alt="抖音二维码"><br><sub><b>抖音 · @耶基大学懂事长</b><br>抖音号 <code>yejiuni</code></sub></td>
    <td align="center"><img src="docs/sponsor/web_wechatauthor.jpg" width="220" alt="作者微信二维码"><br><sub><b>微信 · 扫码加好友</b></sub></td>
  </tr>
</table>

---

[English](README.en.md) · [Français](README.fr.md)

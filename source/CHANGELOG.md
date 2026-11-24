# Changelog

本仓库在固件基线 `v3.29.3` 之上做增量，版本号格式 `v3.29.3-<tag>`，
`tag` 为打包时的日期/批次标识，会写进 `FW_VERSION` 并显示在页面顶部。

## v3.29.3-nr3 — 本地（未推送）

- 修复「首页第一个开关（Nag Killer · 提醒抑制）打不开，重置 NVS 后才能开」：
  `nagCfgLoad()` 遇到旧版/损坏的 NAG 快照会置 `nagNvsReadError`，而保存函数原本
  见到这个锁存就直接拒绝写入，导致开关与设置全部报「Settings could not be saved」。
  现在允许直接用一份校验过的新快照覆盖坏记录，写入成功后再清掉锁存。
- 不再需要整机 factory reset 才能恢复 NAG 开关；其它 NAG 行为未变。

## v3.29.3-nr2 — 本地（未推送）

- EAP 防重启开关改为**默认开启**：变量初值与 NVS 读取默认值都改为 `true`
- 开关的 NVS key 从 `eapNr` 换成 `eapNrOn`，让已刷过 nr1（NVS 里存过关闭）
  的设备升级后也拿到「开启」这个新默认；之后手动关闭仍会正常持久化
- 只改开关默认值，补发逻辑、门控、帧上限、状态上报均未动

## v3.29.3-light1 — 本地（未推送）

两件事：**灯光功能整页换成作者 v3.35.6 的 4 项实现**，以及**EAP 防重启「失败 1」的定位与修复**。

### 1. 灯光功能：作者整页移植 · `feifan_lighting.h` + `lighting_reference_pure.h`

原来「更多 → 灯」只有两项（爆闪 / 强制远光），且是我们自己按作者算法重写的
`feifan_strobe.h`。现在整页换成作者 v3.35.6「设置 → 灯光功能」的 4 项：

| 项 | 做法 |
|---|---|
| 自动驾驶时手动开远光 | AP 中推一次杆切换，走 BODY `0x3E9` DAS 决策位（d1 bit4 + 清 d3 bit0），**不发 0x249** |
| 远光灯频闪 | 作者 Strobe 2：3 s 内连拉三次启动、再拉一次停，亮/灭各 70 ms，走 BODY `0x249` 方波 |
| 后雾灯频闪 | 按住右滚轮 + 左滚轮下滚，触发 20 次、100 ms 半周期，走 BODY `0x273` d2 bit7（**byte0 原样透传**） |
| 动态刹车灯 | 0/1/2 三档，0x145 + 0x39D 两路踏板信号合并，写 BODY `0x3E9` d2 bit6 |

算法逐字拷贝作者的 `lighting_reference_pure.h`（1738 行，无硬件依赖）；
适配层 `feifan_lighting.h` 负责 NVS（`features`/`lighting`）、5 ms 发送任务、
三路 CAN 观察与 TX 闸门。作者自己的 runtime 只实例化了 AP 远光 + 动态刹车灯，
频闪和后雾灯在这里补齐。

- 旧 `q1a/q1b` 开关保留 API 兼容，但读写转发到新模块，不会出现两套实现打架
- 所有发送过 `canTxBarrierMutex` + epoch + 管理员挂起 + USB 被动 + 准入闸门
- 0x249 多主用作者的 `lighting249OwnerPure` 裁定：爆闪占用时转向灯让位
- `/api/lighting/stats` + `/api/lighting/update`，与作者页面同契约
- 词库 986 → 1005

### 2. EAP 防重启「补发 45 帧 · 失败 1」修复

图里的 `失败 1` 不是帧发不出去：`canTxTwaiTransmit()` 用**非阻塞**
`xSemaphoreTake(mutex, 0)` 抢 TX 屏障锁，跟爆闪/AP 注入撞上时返回
`ESP_ERR_TIMEOUT`（0x107）被算成失败。45 帧里 1 次（约 2%）是典型调度让位。

- `ESP_ERR_TIMEOUT` 单列为 `replayBusy`（让位），不计失败、不推进 burst，
  下个 100 ms 周期自动重试
- `lastErr` 从 `int8_t` 改 `int16_t`：TIMEOUT=0x107 原来会被截断成 0x07
- 状态行新增 `让位 N` 和错误名（TX-busy / gate-closed / send-failed）

出包 `v3.29.3-light1`：OTA 1,486,240 B（31%），编译 exit 0 / 0 warning，
页面 JS 10 块全过 `node --check`，sha256 `f855a9ea1aa4eabc8…`（OTA）、
`e6d5a6bde001dede54f…`（32MB 整片）。本地提交，未推送。

## v3.29.3-wifi1 — 本地（未推送）

两件事：**Wi-Fi 中继**（连上 P4 也有网）和**独立抓包页**（抓帧 + 导 CSV）。

### 1. Wi-Fi 中继 · `p4_uplink.h`

原来 P4 只开 AP，手机连上 `P4-CAN` 之后就没有外网了 —— 想开个文档、看个
Release 都得先切回家里 Wi-Fi，再切回来，页面还得重连。

做法是 AP+STA 双模 + lwIP 的 NAPT：

| 点 | 做法 |
|---|---|
| 模式 | `WiFi.mode(WIFI_AP_STA)`，AP 段仍是 `192.168.4.1` 不变 |
| 转发 | `esp_netif_napt_enable(WiFi.AP.netif())`（sdkconfig 里 `LWIP_IP_FORWARD` / `LWIP_IPV4_NAPT` 本来就是 y） |
| DNS | `esp_netif_dhcps_option(ESP_NETIF_DOMAIN_NAME_SERVER)` 把上游 DNS 下发给客户端，兜底 `8.8.8.8` —— 漏了这步会「有 IP、能 ping、解析不了域名」 |
| 落盘 | NVS 命名空间 `wifi`：`up_ssid` / `up_pass` / `up_on` |
| 重连 | 每 15 s 重试一次，状态由 `p4UpLoop()` 每秒轮询，不阻塞 HTTP |
| 关闭 | `WiFi.disconnect(false,false)` + 退回 `WIFI_AP`，不动 AP 本身，页面不掉线 |

**不做**的事：不调 `WiFi.disconnect(true)`、不调 `WiFi.mode(WIFI_OFF)`。本机
Wi-Fi 走 esp-hosted（P4 + C6），把 Wi-Fi 关掉会连 Hosted 传输一起拆掉，
之后页面上就再也连不回来了。

连接本身放在 `loop()` 里发起，不在 HTTP 请求里 `WiFi.begin()`：扫描+关联要几秒，
放请求里会把页面卡到超时。

接口：`GET /api/wifi/uplink` 读状态，`POST /api/wifi/uplink?ssid=…&password=…`
设置上游，`POST /api/wifi/uplink?off=1` 关闭（回包统一是 `{ok, uplink:{…}}`）。

### 2. 独立抓包 · `feifan_capture.h`

LAB 里那套 `can_research_capture.h` 是给「场景标注 + 触发前后窗口」用的，
状态机很重，而且只留触发点附近的行。这次要的是最朴素的东西：按开始、按停止、
把这段整段导出来，所以另起一个环缓冲。

| 项 | 值 |
|---|---|
| 缓冲 | 优先 PSRAM 16384 行（≈256 KB），没有 PSRAM 退内部 RAM 2048 行 |
| 每行 | `ms / id / bus / dlc / data[8]`，16 字节 |
| 过滤 | 按总线掩码（A/B/C）+ 单个 ID |
| 溢出 | 覆盖最旧一行（连续抓不会因为没按停止就丢新帧） |
| 发送 | **一个字节都不发**，纯观察 |

热路径只有一次 `bool` 判断（`if (!ffCapOn) return;`），关着的时候对 CAN 收帧
没有可测影响。写环时进 `portENTER_CRITICAL`：三路总线各有接收任务，两帧在同一
微秒到达是常事，`head` 的自增是读-改-写，不加锁会让两帧抢同一个槽 —— 一条记录里
混着两个 ID 的字节，导出的 CSV 反而看不懂。临界区只有十几次字节拷贝，满负载下
占不到 1% CPU。

接口：`/api/capture/status|start|stop|clear|frames|export.csv`。CSV 按 64 行一批
拷贝、每 8 批让一次 CPU，不在长循环里一直握着自旋锁；行数写在
`X-P4-CAN-CSV-Rows` 响应头里。

### 3. 页面

- 设置页 Diagnostics 之后新增 **Packet Capture** 一行 → 独立面板
  （总线多选、ID 过滤、开始/停止/清空、最近帧、下载 CSV）
- 无线页新增 **WI-FI RELAY · UPSTREAM** 卡片（SSID、密码、连接/停止、状态行）
- 爆闪状态行改成读作者实现（见下一条）
- 词库 965 → 986
- `#capRows` 单独定了等宽字体和 10.5 px 字号：原来继承 16 px，一行 40 多个字符
  会把卡片撑出屏幕

### 4. 修掉上一版留下的两处脏东西

- `web_api.h` 被误转成 LF（仓库 `.gitattributes` 是 `* -text`，一律不转换），
  已恢复 CRLF，diff 从整文件 5700 行回到真实的 +258 行
- 反编译时临时放在仓库根目录的 `_lamp.png` / `_prev_lamp.html` / `_v3356_*.html`
  从版本库移除，本地挪到 `build/refs/`（`build/` 已被忽略）

## v3.29.3-strobe2 — 本地（未推送）

爆闪大灯 / 强制远光**换成作者 v3.35.6 里的实现**，原来那套在车上按不出来。

### 原来为什么按不出来

| 问题 | 原实现 | 作者实现 |
|---|---|---|
| 总线 | `p4CanSendB()` —— 标准 3/Y 方案下这是 CHASSIS | BODY（CAN 1） |
| 触发 | 要求原车 `0x249` 的 `d1` 高 4 位已经是 `0x1x`（已经拉住杆） | 自己拉杆：`d1 bit7..4 = 0x10` |
| 节奏 | 挂在 50 ms 周期任务里翻电平 | 独立时基，亮灭各 70 ms（≈7 Hz） |
| 载荷 | 抄 `S3_STROBE[16][4]` 预计算表 | 按计数器现算 `CRC8(poly 0x2F)` + 16 项异或表 |

前两条任意一条不对，车上就是完全没反应 —— 发到了错误的 CAN 路，且等待一个
永远不会出现的条件。

### 现在的做法（`feifan_strobe.h`）

| 项 | 值 |
|---|---|
| 帧 | `0x249` SCCM_leftStalk，DLC 4，发 BODY |
| 载荷 | `d1 = (模板高2位) \| 0x10 \| 计数器`，`d0 = CRC8(0x2F) 异或 SCCM249_CKSUM_CTR[ctr]` |
| 节奏 | 70 ms 翻一次电平，最长连闪 30 s 自动停 |
| 起停手势 | 3 s 内连拉三次启动，再拉一次停 |
| 模板 | 只用 300 ms 内的新鲜原车帧；过期就退到全 0 基帧（陈旧模板的 `d2/d3` 可能还带着转向灯状态，照抄等于替车主一直打灯） |
| 让位 | 爆闪占着 `0x249` 时转向灯逻辑让位，不抢同一帧 |
| 互斥 | 只有一路任务驱动（`p4CanBusA()==P4_BODY` 或 `p4CanBusB()==P4_BODY`），YL 方案不会双发把频率翻倍 |

`feifan_super3can.h` 里删掉 `S3_STROBE` / `S3_HIGHBEAM` 两张表和对应的 tick 逻辑，
`q1a` / `q1b` 改成转发到 `feifanStrobeSetAuth()` / `feifanStrobeSetHold()`。

新增 `status.strobe`：`auth / armed / hold / tpl / synth / rx / ckBad / ctr / dir /
byte1 / pulls / frames / txOk / txFail`。上车先看 `rx` 与 `tpl`：`rx=0` 说明这车
根本不周期发 `0x249`，那爆闪就没有触发源。

## v3.29.3-eap2 — 本地（未推送）

「更多功能」新增第 8 项 **EAP 防重启**（`eapNr`），独立一个开关，落 NVS。

### 要解决的现象

「被封车辆 + TLSSC 恢复」开着时，每轮上电都要人工重启一次车机才有 TLSSC。
原因是 `0x331` 的恢复只做读-改-重发：车发一帧我们改一帧（`doInjectTlsscRestore`），
而车机只在自己启动时读一次 DAS 配置并锁存 —— 那一刻总线上没有改过的 `0x331`，
就锁存「封禁」。上游 Flipper 项目文档也直接把它标成 "Triggers MCU reboot"。

### 做法（`feifan_eap_norestart.h`）

| 步骤 | 说明 |
|---|---|
| 缓存 | 补丁后的 `0x331` 载荷存 NVS（`feifan/eapHex`），每上电周期最多写一次 |
| 补发 | CAN B 就绪后开 60 s 窗口，10 Hz 重发缓存帧，不等车先发 |
| 让位 | 车自己发 `0x331`（2 s 内新鲜）就停手，回退到读-改-重发 |
| 收尾 | 窗口到期或 600 帧后彻底停手 |

门控：只在「被封车辆」+「TLSSC 恢复」都开时动作；走 CAN B；过 TX 屏障 /
管理员挂起 / USB 被动 / epoch 检查。无缓存帧时什么都不发。

### 页面

- 更多功能页新增 `EAP` 分区（1 项 · 存入 flash），开关 + 状态行
- 状态行分五种：关闭 / 缺前提 / 待首帧 / 车在自己发 / 补发 N 帧，附缓存帧 hex
- 新增 `feed`：`status.eapNr`（enabled / prereq / cacheValid / cacheHex /
  stockRx / replayOk / replayFail / burst / lastErr）
- 词库 961 → 965

### 工具链修正

- `pack_ota.py` 不再在打包时跑 `gen_feifan.py` / `integrate_feifan.py`：
  这两个脚本会按 `data/feifan-menu-data.json` 重建整页（141 项 8 组），
  把手改的 3 组 7 项 + EAP 分区整块覆盖。页面现在以 `dashboard_source.html` 为准。

## v3.29.3-more1122dup2 — 本地（未推送）

「更多功能」里凡是设置页已经有的，全部删掉。之前那 11 行不是新能力，
只是同一批后端函数的第二个入口：拨哪个开关，落到车上都是同一个结果。

### 删掉的 11 项（附设置页对应位置）

| 删掉的项 | id | 设置页里的同一项 |
|---|---|---|
| Nag Killer 抑制 | `d1a` | Settings → Nag Killer（`panelNag`，同一份 `nagCfg`） |
| 脱手检测抑制 | `a1d` | 原生层里与 `d1a` 同一个分支 |
| 打灯变道 | `d1b` | Settings → LAB → Auto Lane Change Enable |
| 注视提醒抑制 | `a1a` | Settings → Disable Driver Monitoring |
| ISA 提示音抑制 | `a1b` | Settings → ISA Suppression |
| 禁用视觉限速 | `a1c` | Settings → Visual Speed Control |
| EAP 增强 | `a3a` | Settings → Auto Blinker（`setAutoBlinkerEnabled`） |
| 召唤心跳 | `a3d` | Settings → LAB → Summon Heartbeat Override |
| 深色 / 跟随系统 / 定时深色 | `s1a`/`s1b`/`s1c` | Settings → Appearance |

后三项本来就没接线（`s1a`..`s1c` 在页面脚本里没有任何处理），纯装饰开关。

### 页面

- 5 组 18 项 → **3 组 7 项**：驾驶辅助 2 · 灯光 2 · 车身与动力 3
- 统计条现在就是 `7 P4 原生 · 7 项可用`；「本地 UI」档位随最后 3 项一起消失
- 图例从 4 条减到 3 条（去掉「本地 UI」），证据等级只剩 A，文案改成实际含义
- 首页副标题改成「只列设置页没有的项」；设置页里「更多功能」那行的旧描述
  `141 entries in 8 groups` 也改成 `7 entries in 3 groups`
- 分组右侧 `N/M` 把一次性按钮（维护模式 `q1c`）也算作已就绪，
  否则车身与动力永远停在 2/3

### 后端

- 七个 q1x 的行为不变；`POST /api/feifan/config` 仍接受被删的 id，
  老客户端与外部脚本不会突然 404，只是页面不再暴露
- 模拟后端 `FF_NATIVE` 同步收敛到 q1x

### 交付

- 本地出包 `v3.29.3-more1122dup2`：OTA 1,460,224 B（31%），页面 512,881 B / gzip 131,891 B，
  编译零 warning，sha256 `6d3dc5b9…`（OTA + 32MB 整片 + manifest），
  **未推送、未发 Release**。

## v3.29.3-more1122svc — 本地（未推送）

对照官方最新源码（`P4can-universal-unlock` v3.35.3），把 v3.35.0 新增的 **Service mode**
按官方语义接进来，替换掉之前从 SUPER_3CAN 搬的 q1c。

### 与官方的差异（已修正）

| | 之前（SUPER_3CAN） | 现在（官方 v3.35.0） |
|---|---|---|
| 载荷 | `0x339 = 00 00 00 00 00 E0 00 00` | 全 0，仅 `data[5] = 0x80` |
| 次数 | 连发 3 次 | 每次点击 1 帧 |
| 总线 | CAN B | BODY（CAN 1） |
| 闸门 | 无 | TX 屏障 / 管理员挂起 / USB 被动 / 三路运行 / epoch / Summon 准入 |

`data[5]` 的 bit5/bit6 在官方源码里没有依据，按官方只置 bit7。

### 新增 `feifan_service.h`

- `feifanServiceEnter()`：单帧 0x339，失败不重试、不重放、不写 NVS
- `feifanServiceObserve()`：BODY 路观察 `0x284 UI_vehicleModes` 的 `d0 bit3`，回读车辆是否真的进去了
- `status.serviceConfig`：`txOk / txFail / lastMs / lastErr / confirmed / confirmMs / rx284 / rx339`

### 页面

- 「车身与动力」拆出 **维护（Service）** 小节，一行按钮「进入维护模式」
- 按钮下方状态行直接给结论：已发送 / 车辆已进入维护模式 / 闸门关闭 / 发送失败，不再依赖已删掉的快捷条
- `POST /api/feifan/action` 的 `q1c`（兼容 `svc`）改回单帧，回 `service-frame-sent` / `tx-gate-closed`
- 词库 948 → 955

出包 `v3.29.3-more1122svc`：OTA 1,460,640 B（31%），本地提交，未推送。

## v3.29.3-more1122s3d — 本地（未推送）

给爆闪加**上车自检**：`status.s3Config` 增加 `stalkByte` / `stalkRx`。

- `stalkRx`：开机以来收到的 `0x249` 帧数。不碰杆也在涨，说明这车是周期帧，爆闪才可能成立；
  一直是 0 就说明这车的 `0x249` 不在这条总线上（或只在操作杆时发），别白费劲。
- `stalkByte`：最近一帧的 `data[1]`。远光位应该是 `0x1x`，杆回位应回到 `0x0x`。
  推杆后这个值不变 = 杆位编码和判断条件对不上，需要重新定条件。
- 这两个观察量即使开关全关也记录（每帧两个字节，10 Hz 量级），其余逻辑仍保持全关零开销。
- 页面 `probe()` 的 s3Config 合并跳过新字段，避免写进 localStorage。

出包 `v3.29.3-more1122s3d`：OTA 1,458,704 B（31%），本地提交，未推送。

## v3.29.3-more1122s3c — 本地（未推送）

修正爆闪大灯（`q1a`）的触发节奏，之前照搬原工程时把计数器清零了。

原工程 `BSLED_JS` 到 15 后**不再清零**，此后每来一帧原车 `0x249` 就回发一条查表帧；
移植版当时写成「发一条就归零」，结果 15 帧才闪一下，等于没闪。
现在改成 `s3StrobeTick` 到 15 后锁存（`< 15` 才自增），满足条件后逐帧回发，
`S3_STROBE` 的奇偶项（`byte1` 高 4 位 `0x0x` / `0x1x`）一开一关，形成交替爆闪。

- `status.s3Config` 增加 `strobeArm`：远光杆是否已按满 15 帧、爆闪是否放行
- 页面文案改为可直接照做的动作说明：「打开开关后把远光杆保持在远光位约 1.5 秒」
- 词库 947 → 948

出包 `v3.29.3-more1122s3c`：OTA 1,458,528 B（31%），本地提交，未推送。

## v3.29.3-more1122s3b — 本地（未推送）

第二轮纠正：上一版还在页面上保留「需模块 / 需新增 / 待固件 / 不可达」四类共 123 条，
开关拨不动、拨了也不生效。本轮**把这些行全部删掉，不再展示**，只留真能落地的东西；
同时把刚开源的 SUPER_3CAN 工程里 P4 没有的 7 项车控能力移植进来。

### 新增：SUPER_3CAN 移植（`feifan_super3can.h`）

原工程是 STM32F103 + 双 MCP2515 的三路 CAN 盒子，做法是「读 stock 帧 → 改字节 →
按帧自带计数器 `data[1] & 0x0F` 查 16 项预计算表 → 回发」，不逆推校验算法。
数据表与触发条件照抄进本固件：

- `q1a` 爆闪大灯 `0x249`：远光位连续 15 帧后取 `S3_STROBE[index]`
- `q1b` 强制远光 `0x249`：取 `S3_HIGHBEAM[index]`
- `q1c` SERVICE `0x339`：`00 00 00 00 00 E0 00 00` × 3，一次性按钮（`POST /api/feifan/action`）
- `q1d` 越野 `0x2B6 = 60 00 00`、`q1e` ESP off `0x2B6 = 46 00 00`：2 帧 × 3 轮
- `q1f` 动力狂暴 `0x334`：`byte0 = 0xFF` 后补加和校验
- `q1g` 滚轮换挡 `0x229`：车速 ≤ 8 km/h 时取 `S3_GEAR_R/N/D/P[index][3]`

约束：只在收到对应 stock 帧时才回发；开关全关时 `feifanS3Observe()` 头一行返回，零开销；
不写 NVS，熄火重启回到关闭。

### 新增：状态与路由

- `GET /api/feifan/status` 增加 `s3Config`：`q1a..q1g` 开关、`gearState`、`speedKph`、`txOk`、`txFail`
- `POST /api/feifan/config` 由三段路由扩为四段（P4 原生 → SUPER_3CAN → TSL 模块 → 仅持久化），
  移植项回 `effect=applied, via=p4-super3can, sessionOnly=true`
- `POST /api/feifan/action` 新增 `q1c` 分支，回 `service-pulse-sent` / `can-tx-failed`

### 页面：删除 123 条不可用项

- 首页从 8 组 141 项收成 **5 组 18 项**：驾驶辅助 4 · 灯光 2 · 免打扰与 AP 6 · 车身与动力 3 · 应用设置 3
- 统计条只渲染非零档：`15 P4 原生 · 3 本地 UI · 18 项可用`；
  删掉「需模块 / 需新增 / 待固件 / 不可达」全部档位、灰锁样式与图例
- 连带清掉：5 键快捷条、`One-tap actions`、`Integration notes`、`panelFeifanDev` 集成清单、
  不可达的 `panelFeifanHome`（仪表与快捷）和只剩一条死行的 `panelFeifanComf`（舒适）
- 图例改成 4 条且名副其实：`P4 原生` / `证据等级 A·B·C` / `本地 UI` / `仅内存`
  （原来把证据等级 `B` 当成「仅本次会话」，是标错的）
- 中英词库 893 → 947 条，补齐爆闪 / 远光 / 越野 / ESP / 动力狂暴 / 滚轮换挡 / SERVICE 等新条目

### 仍未验证

- SUPER_3CAN 的查表值来自开源工程源码，**没有实车对照**；`0x2B6` / `0x339` / `0x334`
  动的是动力与制动域，先在封闭场地试。
- 原生 15 项均未实车写入验证；`gauge` 仍是 503。

### 交付

- 本地出包 `v3.29.3-more1122s3b`（OTA + 32MB 整片 + manifest），**未推送、未发 Release**。

## v3.29.3-more1120native — 本地（未推送）

前提纠正：这台 P4 **没有外挂 TSL 模块**。上一版把 44 个模块配置位域算进「72 可用」，
在无模块实机上其实是死链路。本轮方向改为「移植进我们的固件」——能由 P4 自己发 CAN 帧的
走原生路径，其余如实标注。

### 新增

- **`feifan_native.h`（新文件）**：P4 原生映射层，把 10 个页面 id 1:1 接到
  `vehicle_logic.h` / `continuous_ap_runtime.h` 里**已存在**的设置函数；
  只接线，不复制 CAN 逻辑、不新造帧、不依赖外挂硬件
  - `d1a`/`a1d` Nag Killer、`a1b` ISA 抑制、`d1c` 加速模式、`d1b` 打灯变道 0x293、
    `a1a` 驾驶员监测、`a1c` 视觉限速、`a3a` EAP / 自动转向灯、
    `a3d` 召唤心跳开关、`a3e` 召唤心跳值
  - `a2f`（AP 自动恢复）页面取值是 TSL 配置块语义，与 `ContApMethod` 不等价，**不硬映射**
  - 会话级项（`d1c` / `a3d` / `a3e`）不写 NVS，避免重进页面留下假状态
- **`feifan_api.h` 改为原生优先**：`POST /api/feifan/config` 三段路由
  （P4 原生 → TSL 模块 → 仅持久化），原生项回 `effect=applied, via=p4-native`
  - `GET /api/feifan/status` 新增 `applied`（10 项原生清单）、`nativeConfig`（权威真值）、
    `moduleAbsent`（未启用链路且未读到配置块 = 车上没装模块）
  - 无模块时模块项回 `stored + note=module-absent`，快捷动作回 `501 / reason=module-absent`，
    **不再因为点一次按钮就自动拉起 BLE 扫描**
- **页面诚实标注**：统计条由旧「可用 / 需新增 / 不可达」三档改为六档
  `P4 原生 10 · 本地 UI 12 · 需模块 44 · 需新增 61 · 待固件 6 · 不可达 8`（合计 141）
  - 无模块时模块项挂琥珀「需模块」并禁用控件；装了模块后重算为蓝色「模块」并恢复控件
  - 探活 / 写入 / 切语言后通过 `ff-render` / `ff-link-ok` 事件重算统计，不再只算一次
  - 写入回执区分「已下发到 P4 固件（原生发帧）」「已下发到模块」「需 TSL 模块」「等待固件」
  - 页面副标题与中英词库同步到新口径，去掉「19 已有 / 101 需新增 / 9 不可达」旧描述
- **模拟页同契约**：`build_sim.py` 默认 `moduleAbsent=true`（与无模块实机一致），
  `applied` 为 10 项原生，新增 `nativeConfig` / `via` / `moduleAbsent`；
  SIM 面板可勾选「模拟车上装了 TSL 模块」验证两种分支

### 核查结论（B 批）

- `0x229` / `0x145` / `0x118` 在 P4 基线里主要是 Continuous AP 的**接收解码**，不是换挡发送；
  `0x30A` 命中 0；`0xB0` 是模块 BLE 回包而非车端 CAN 帧。
  没有可靠车端帧映射，**不硬造发送逻辑**，相关项保持「需新增 / 待固件」并注明需实车抓包。
- 9 项座椅 / HVAC 类走座舱以太网或专用总线，三路 CAN 物理够不到，保持锁定。

### 仍未验证

- 原生 10 项与模块位域均未实车写入验证；首次上线先看 `status.nativeConfig` / `status.applied`。
- `gauge` 仍是 503。

### 交付

- 本地出包 `v3.29.3-more1120native`（OTA + 32MB 整片 + manifest），**未推送、未发 Release**。

## v3.29.3-more1117r3 — 2026-11-17（已发布 Release）

端点层从 TODO 变成真的接线，但只接到能接的部分。

### 新增

- **`feifan_api.h`**：`/api/feifan/{status,config,blob}` 三个端点落地，
  随 `web_api.h` 注册；设置在控制器 NVS 里持久化，不再只存手机 `localStorage`
- **逐项如实回执**：`POST /api/feifan/config` 返回 `effect=applied|stored`，
  页面拿到 `stored` 就标「待固件」，不再出现「开关能拨、实际没动」
- **真正生效的三项**：`d1a` Nag Killer、`a1b` ISA 抑制、`d1c` 加速模式，
  直接调固件已有的 `nagCfgSaveCandidate/Commit`、`isaSuppressionControlApply`、
  `requestPedalMapMode`，没有新造位域
- 页面顶部状态由 `SYNC` 改为 `已连接 / LINKED`，说明是「写入控制器 NVS」；
  已标注「现有」但没有车控通道的行挂琥珀色「待固件 / Pending」小标
- 模拟页假后端改成与固件同契约（action 501 / gauge 503 / applied 三项），
  默认按 **三路 CAN 在线**（`canOnline:[1,1,1]`）呈现，与真机行驶时一致
- `GET /api/feifan/status` 增加 `canOnline` / `canRx` / `canTxOk` / `canTxFail`
  四组数组，取自 `p4CanOnline` 等真实计数器，不猜
- 页面「固件连接」下方新增 **CAN 总线** 行：`3CAN OK` 绿色标签 +
  `A✓ · B✓ · C✓ · RX 3173`；未连接或固件未上报时显示 `—` 并说明原因

### 追加：TSL 模块 BLE 客户端（真正的发送通道）

- **`tsl_ble.h`**：直连 TSL 模块（服务 `FFF0` / 特征 `FFF1`），实现帧
  `55 7F CMD LEN_H LEN_L payload SUM`、20 B 分块串行写、通知重组与校验，
  以及 `0xAB` 读整块配置 / `0xA3` 整块回写 / `0xA7` 车身动作 / `0xBB` 键值
- **`tsl_map.h`**：44 条「页面条目 → byte/shift/mask」映射，
  由 `data/feifan-menu-data.json` 的实现注解与 `fields_full` 交叉校验后生成；
  含 `bias`（轻鸣笛次数页面从 1 起算）与 `maxOut`（模块只收 0..2 的字段拒绝写 3）
- **`POST /api/feifan/config` 新增一条通道**：命中映射就走模块，
  真写回 `effect=applied`，没连上就 `stored` + `pending` + `note=<链路状态>`
- **`POST /api/feifan/action` 不再一律 501**：模块在线且给的是数字码才发，
  否则 501 并回 `reason=module-not-ready` / `action-code-missing`
- **新增 `GET/POST /api/feifan/module`**：手动开关模块链路，返回扫描/连接/握手/就绪状态
- **`GET /api/feifan/status` 增加 `module` 与 `moduleConfig`**：链路状态、
  已读配置块长度、真实取值；页面以模块回读为准
- 页面：46 项解锁并改挂蓝色「模块 / Module」标（原来一律「需新增」），
  首次点会顺手拉起链路，回执分「已下发到固件 / 已保存 · 正在连接模块」；
  统计从 31 可用 / 101 需新增 / 9 不可达 变为 **72 可用 / 61 需新增 / 8 不可达**
- 模拟页假后端同步：`module.ready=true`，模块条目回 `applied`，
  动作回 200，`/api/feifan/module` 可切换在线/离线

**互斥说明**：BLE 由 C6 协处理器提供，S3XY 映射开着时模块链路直接报 `ble-busy`，
不抢同一个 host。**未验证**：帧与位域是静态逆向结果，需一次实车抓包复核。

### 未完成（页面已标注）

- `GET /api/feifan/gauge` → 503：0xB0 仪表、0x5D9 胎压载荷尚未解码
- `GET /api/feifan/gauge` → 503：0xB0 仪表、0x5D9 胎压载荷尚未解码
- `blob` 的 `quick` / `config`（模块 256 B 快照）无来源，返回空 + pending；
  `backup` 可用

## v3.29.3-more1117r2 — 2026-11-17

修订版，替换 `more1117`。

### 新增

- 「更多功能」页副标题下增加开源项目地址（可点击）
- **补齐实验页中文**：新增 147 条词条，覆盖研究工具列表、R79 AP 控制、召唤心跳覆盖、
  自动变道使能、CAN 研究抓包、驾驶员监测抓包、车窗测试等全部子面板
- 运行时状态兜底：`#r79TxBig` 等 5 个节点由原版 JS 周期性写回英文
  （`Waiting` / `Active · AP engaged`），中文模式下按 `·` 分段查表纠正
- README 顶部加 32 MB / 16 MB 警告：16 MB 板子刷入会变砖，需自行改分区表与 FQBN
- README 增加赞助与作者联系方式（抖音 / 微信）

### 修正

- 实验页 `Driving State`、`Research Tools`、`Gear` 等静态文案漏译
- 中文词条 380 → 527 条

## v3.29.3-more1117 — 2026-11-17

### 新增

- **「更多功能」页**（`data-page="feifan"`），8 组 141 项：
  驾驶辅助 24 / 灯光与喇叭 34 / 舒适与座椅 15 / 免打扰与 AP 34 /
  车身与开合 14 / 仪表与快捷 9 / 模块设置 11 / 集成清单
- 每条功能带证据等级（`A` 实锤 / `B` 推断 / `C` 本地）、落地状态与帧/位域追溯
- 44 个 TSL 配置位域的读写封装，统一走 `CMD 0xA3` 整块回写
- 一键快捷条：前爆闪 `0x5B`、后爆闪 `0x5E`、脉冲喇叭 `0x5A`、领航 `0x5F`、
  电池预热 `0x1D/0x1C`，每次点击在状态行回显实际发出的字节
- 固件更新面板增强：分区余量、当前版本、拖拽、文件预检、二次确认、
  进度与错误码中文释义
- 中英双语覆盖整页，技术标识（帧 ID / 位域 / API / FQBN）保持原样
- 本地模拟器 `sim/p4can-sim.html`：假 `/api/feifan/*`、假 OTA 进度、请求日志
- 工具链：`build_more.py` / `gen_feifan.py` / `integrate_feifan.py` /
  `patch_ota.py` / `pack_ota.py` / `build_sim.py` / `gen_docs.py`

### 修复

- 快捷按钮点了没反馈：原来只有 420 ms 的 `scale(.9)`，真机上很难察觉。
  改为按钮下方常驻状态行，三态（已发送 / 仅记录在本机 / 再点一次确认）
- 状态行永远不变色：CSS 选择器写 `[data-kind="ok"]`，JS 传 `sent`，已统一
- 中文回译撞车：「闪 2 次」被翻回「2 次」，删掉裸词条并区分次数标签
- 位域提示串漂移：加字段后旧的 `TECH_OVERRIDE` 把 `byte25 >>> 0 & 3` 盖成
  `0x3E9 · DAS`，只保留 `d2a` / `c1a` 两条覆盖
- `Nag Killer` 与看板已有词条重名，改为 `Nag Killer 抑制 / suppression`
- 系统升级入口从「更多功能」页移除，只保留在默认设置里，避免两处各刷一半
- OTA 前端异步竞态：`FileReader.onload` 晚于确认超时，导致误报"文件错误"，
  在 `onload` 里先解除待确认态

### 已知问题

- `web_api.h` 中 `/api/feifan/{status,config,action,gauge,blob}` **未实现**，
  页面写入只落 `localStorage`，不进 CAN
- 设计图的 `0x249 / 0x10C / 0x3E2 / 0x3E3` 在固件中不存在，相关功能已按位域重写
- 全部写入行为未经实车验证

## 基线

- `v3.29.3` — 上游 P4can-universal-unlock，本仓库从这一版开始改动

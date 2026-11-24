// ─────────────────────────────────────────────────────────────
// TLSSC 免重启（TLSSC anti-restart）—— 「更多功能」里的一个独立开关
//
// 要解决的问题
// ------------
// TLSSC 恢复（vehicle_logic.h 的 doInjectTlsscRestore）只做一件事：
// 收到车自己发的 0x331 DAS_autopilotConfig → 把 byte0 低 6 位改成
// 0x1B（SELF_DRIVING）→ 原帧照发 + 我们再补一份改过的。
//
// 这是「车发一帧、我们改一帧」的旁路改写，不是主动造帧。而车机（MCU）
// 只在自己启动时读一次 DAS 配置并锁存：如果那一刻总线上还没有被改过的
// 0x331，车机就锁存「封禁」，必须人工重启车机让它重读 —— 于是每轮上电
// 都要捅一次，这正是用户抱怨的那个现象。上游 Flipper 项目的文档也把
// TLSSC Restore 直接标成 "Triggers MCU reboot"。
//
// 本开关做的事情
// --------------
//   1) 把「上一次成功补丁过的 0x331 载荷」缓存到 NVS（每个上电周期最多
//      写一次，避免频繁擦写）。断电后仍在，下一轮上电即可用。
//   2) 每次上电（CAN B 就绪）后开一个 60 秒窗口，以 10 Hz 把这条缓存帧
//      补发回总线 —— 不等车先发。这样车机在启动读配置的任何时刻，总线上
//      都已经有一条 SELF_DRIVING 的 0x331。
//   3) 一旦车自己开始发 0x331（2 秒内有新鲜帧），立刻停手，回到原来的
//      「跟随原厂 + 读改重发」模式，不跟真帧抢总线。
//   4) 窗口到期或补发次数到上限就彻底停手，不会无限期造帧。
//   5) 开关默认开启；v3.29.3-nr2 起换新 NVS key，旧版升级后也是开启，
//      用户随时可以在页面里关掉（关掉后的状态同样持久化）。
//
// 边界与安全
// ----------
//   · 只在「被封车辆」+「TLSSC 恢复」都打开时才动作。少了任何一个都
//     直接返回，绝不替用户决定去改 DAS 配置。
//   · 只走 CAN B（chassis / VH），与 0x331 的原生路由一致。
//   · 过和普通注入一样的闸门：TX 屏障、管理员挂起、USB 被动、epoch。
//   · 没有缓存帧（首次使用）时什么都不发，页面显示「等待首帧」，
//     不做无依据的合成帧。
//   · 所有发送失败只计数，不阻断 CAN 任务。
// ─────────────────────────────────────────────────────────────
#pragma once

#define FEIFAN_EAP_NR_NS      "feifan"   // 与 feifan_api.h 同一个命名空间
// v3.29.3-nr2 起默认开启。换新 key 让旧版（默认关闭、NVS 里存过 eapNr=0）
// 升级后也拿到「开启」这个新默认；用户之后在页面里关闭仍会正常持久化。
#define FEIFAN_EAP_NR_KEY_ON  "eapNrOn"  // 开关本身（跨上电保持）
#define FEIFAN_EAP_NR_KEY_HEX "eapHex"   // 缓存的 0x331 载荷（8 字节 hex）

#define FEIFAN_EAP_NR_WINDOW_MS      60000u  // 每次上电的补发窗口
#define FEIFAN_EAP_NR_PERIOD_MS        100u  // 补发周期 = 10 Hz
#define FEIFAN_EAP_NR_MAX_FRAMES       600u  // 单次上电最多补发帧数
#define FEIFAN_EAP_NR_STOCK_FRESH_MS  2000u  // 车自己在发：2 秒内有帧就让位

static volatile bool eapNrEnabled = true;       // 开关（NVS 持久化，默认开启）
static uint8_t eapNrFrame[8] = {};              // 缓存的那条 0x331（已补丁）
static volatile uint8_t eapNrFrameLen = 0;
static volatile bool eapNrFrameValid = false;
static volatile bool eapNrPersisted = false;    // 本轮上电是否已写过 NVS
static volatile uint32_t eapNrStockRx = 0;      // 收到的车端 0x331 帧数
static volatile uint32_t eapNrStockMs = 0;      // 最近一次车端 0x331 的时刻
static volatile uint32_t eapNrReplayOk = 0;
static volatile uint32_t eapNrReplayFail = 0;
static volatile uint32_t eapNrReplayBusy = 0;   // TX 屏障锁被别的注入占用（让位，非真失败）
static volatile uint32_t eapNrReplaySkip = 0;   // 因门控/缺数据跳过的周期数
static volatile uint32_t eapNrLastReplayMs = 0;
static volatile int16_t eapNrLastErr = 0;   // esp_err_t 高字节非零（如 TIMEOUT=0x107），int8_t 会截断
static volatile uint32_t eapNrArmedMs = 0;      // 窗口起点（CAN B 首次就绪）
static volatile uint16_t eapNrBurst = 0;        // 本轮已补发帧数

// ── NVS ───────────────────────────────────────────────────────
static void feifanEapNrLoad() {
  Preferences p;
  if (!p.begin(FEIFAN_EAP_NR_NS, true)) return;
  eapNrEnabled = p.getBool(FEIFAN_EAP_NR_KEY_ON, true);
  String hex = p.getString(FEIFAN_EAP_NR_KEY_HEX, "");
  p.end();

  if (hex.length() == 16) {
    bool ok = true;
    for (uint8_t i = 0; i < 8; i++) {
      const char hi = hex[i * 2], lo = hex[i * 2 + 1];
      auto nib = [](char c) -> int8_t {
        if (c >= '0' && c <= '9') return (int8_t)(c - '0');
        if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
        return -1;
      };
      const int8_t a = nib(hi), b = nib(lo);
      if (a < 0 || b < 0) { ok = false; break; }
      eapNrFrame[i] = (uint8_t)((a << 4) | b);
    }
    if (ok) {
      eapNrFrameLen = 8;
      eapNrFrameValid = true;
    }
  }
  eapNrPersisted = eapNrFrameValid;  // 已从 NVS 读回，本轮不必再写同一条
}

// 每个上电周期最多写一次：0x331 的计数器每帧都变，逐帧落盘会把 flash 写废。
static void feifanEapNrPersistFrame() {
  if (eapNrPersisted || !eapNrFrameValid) return;
  char hex[17];
  for (uint8_t i = 0; i < 8; i++)
    snprintf(hex + i * 2, 3, "%02X", eapNrFrame[i]);
  hex[16] = '\0';
  Preferences p;
  if (!p.begin(FEIFAN_EAP_NR_NS, false)) return;
  const size_t n = p.putString(FEIFAN_EAP_NR_KEY_HEX, hex);
  p.end();
  if (n > 0) eapNrPersisted = true;
}

// ── 开关写入：落 NVS，重启后仍然是用户选的状态 ────────────────
// 这是「更多功能」里唯一一个跨上电保持的项 —— 它的全部意义就是
// 「以后每轮上电都不用人再点一次」，所以必须持久化。
static bool feifanEapNrSet(bool on, const char *&note) {
  note = "applied";
  Preferences p;
  if (!p.begin(FEIFAN_EAP_NR_NS, false)) {
    note = "nvs-open-failed";
    return false;
  }
  const size_t n = p.putBool(FEIFAN_EAP_NR_KEY_ON, on);
  p.end();
  if (n == 0) {
    note = "nvs-write-failed";
    return false;
  }
  eapNrEnabled = on;
  // 打开时重新开窗：用户在车上刚点开，应该立刻开始补发，而不是等下一轮上电。
  // 若本轮已经观察过 0x331（缓存里有 RAM 副本），顺手补一次 NVS，下次上电即可用。
  if (on && eapNrFrameValid && !eapNrPersisted) feifanEapNrPersistFrame();
  eapNrArmedMs = 0;
  eapNrBurst = 0;
  eapNrStockMs = 0;
  return true;
}

// ── 车端 0x331 观察：记录「车确实在自己发」，并缓存补丁后的载荷 ──
// 由 can_runtime.h 的 CAN B 分发表调用（紧跟在 doInjectTlsscRestore 之后）。
// 注意：开关关着也要缓存（只写 RAM）。否则用户第一次打开时会因为「还没有
// 缓存帧」而白等一轮上电；关着时缓存不发任何帧，没有副作用。
static void feifanEapNrObserve331(const twai_message_t &f) {
  if (f.extd || f.rtr || f.data_length_code < 1) return;
  eapNrStockRx++;
  eapNrStockMs = (uint32_t)millis();
  // 只缓存我们真正要用的形状：byte0 高 2 位保留、低 6 位强制 SELF_DRIVING。
  uint8_t patched[8] = {};
  const uint8_t n = f.data_length_code > 8 ? 8 : f.data_length_code;
  for (uint8_t i = 0; i < n; i++) patched[i] = f.data[i];
  patched[0] = (uint8_t)((patched[0] & 0xC0U) | 0x1BU);
  bool changed = (eapNrFrameLen != n) || !eapNrFrameValid;
  for (uint8_t i = 0; i < n && !changed; i++)
    if (eapNrFrame[i] != patched[i]) changed = true;
  if (changed) {
    for (uint8_t i = 0; i < n; i++) eapNrFrame[i] = patched[i];
    eapNrFrameLen = n;
    eapNrFrameValid = true;
    // 只有开关打开时才落 NVS：没用这个功能的车不该为它写 flash。
    if (eapNrEnabled) feifanEapNrPersistFrame();
  }
}

// ── 周期性补发：由 CAN B 任务的 service 段调用 ─────────────────
static void feifanEapNrTick() {
  if (!eapNrEnabled) return;
  // 只对被封禁车 + 已开 TLSSC 恢复的场合有意义；少一个都不动手。
  if (!bannedCar || !tlsscRestoreEnabled) return;
  if (!activeCanBIsChassis()) return;
  if (!twaiReady || canTxAdministrativeHold || canUsbPassive()) return;

  const uint32_t now = (uint32_t)millis();
  if (eapNrArmedMs == 0) eapNrArmedMs = now ? now : 1u;
  if ((uint32_t)(now - eapNrArmedMs) > FEIFAN_EAP_NR_WINDOW_MS) return;
  if (eapNrBurst >= FEIFAN_EAP_NR_MAX_FRAMES) return;
  if (!eapNrFrameValid) { eapNrReplaySkip++; return; }

  // 车自己在发 0x331 就让位：读改重发那条路已经覆盖，不要和真帧抢总线。
  if (eapNrStockMs && (uint32_t)(now - eapNrStockMs) < FEIFAN_EAP_NR_STOCK_FRESH_MS)
    return;
  if (eapNrLastReplayMs && (uint32_t)(now - eapNrLastReplayMs) < FEIFAN_EAP_NR_PERIOD_MS)
    return;

  twai_message_t out = {};
  out.identifier = 0x331;
  out.extd = 0;
  out.rtr = 0;
  out.data_length_code = eapNrFrameLen;
  for (uint8_t i = 0; i < eapNrFrameLen; i++) out.data[i] = eapNrFrame[i];

  const uint32_t epoch = canTxEpochSnapshot();
  const esp_err_t err = canTxTwaiTransmit(&out, epoch);
  eapNrLastErr = (int16_t)err;
  if (err == ESP_ERR_TIMEOUT) {
    // TX 屏障锁被别的注入短暂占用（爆闪/AP/维护等），本周期让位。
    // 这不是「帧发不出去」：不推进节流时间戳、不计入 burst，下一个
    // 100 ms 周期自然重试。旧版把它当失败上报，页面上就出现「失败 1」。
    eapNrReplayBusy++;
    return;
  }
  eapNrLastReplayMs = now ? now : 1u;
  eapNrBurst++;
  if (err == ESP_OK) eapNrReplayOk++;
  else eapNrReplayFail++;
}

static String feifanEapNrConfigJson() {
  String j = "{";
  j += "\"enabled\":"; j += eapNrEnabled ? "true" : "false";
  j += ",\"prereq\":";  // 页面据此提示「还要开哪两个开关」
  j += (bannedCar && tlsscRestoreEnabled) ? "true" : "false";
  j += ",\"banned\":"; j += bannedCar ? "true" : "false";
  j += ",\"restore\":"; j += tlsscRestoreEnabled ? "true" : "false";
  j += ",\"cacheValid\":"; j += eapNrFrameValid ? "true" : "false";
  j += ",\"cacheLen\":"; j += String((unsigned)eapNrFrameLen);
  j += ",\"cacheHex\":\"";
  if (eapNrFrameValid) {
    for (uint8_t i = 0; i < eapNrFrameLen; i++) {
      char b[3];
      snprintf(b, sizeof(b), "%02X", eapNrFrame[i]);
      j += b;
    }
  }
  j += "\",\"stockRx\":"; j += String((unsigned long)eapNrStockRx);
  j += ",\"stockMs\":"; j += String((unsigned long)eapNrStockMs);
  j += ",\"replayOk\":"; j += String((unsigned long)eapNrReplayOk);
  j += ",\"replayFail\":"; j += String((unsigned long)eapNrReplayFail);
  j += ",\"replayBusy\":"; j += String((unsigned long)eapNrReplayBusy);
  j += ",\"replaySkip\":"; j += String((unsigned long)eapNrReplaySkip);
  j += ",\"burst\":"; j += String((unsigned)eapNrBurst);
  j += ",\"lastMs\":"; j += String((unsigned long)eapNrLastReplayMs);
  j += ",\"lastErr\":"; j += String((int)eapNrLastErr);
  j += ",\"lastErrName\":\"";
  // 0x107 = ESP_ERR_TIMEOUT（锁忙，已单列 busy）；0x103 = INVALID_STATE；
  // 0x10B = ESP_FAIL。页面直接显示名字，不再只给一个截断的数字。
  switch ((uint16_t)eapNrLastErr) {
    case 0: j += "OK"; break;
    case 0x107: j += "TX-busy"; break;
    case 0x103: j += "gate-closed"; break;
    case 0x10B: j += "send-failed"; break;
    default: j += "other"; break;
  }
  j += "\",\"epoch\":\""; j += canTxEpochSnapshot() ? "live" : "boot";
  j += "\"}";
  return j;
}

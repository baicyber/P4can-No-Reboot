#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// 远光爆闪 / 强制远光 —— 移植自作者 v3.35.6（Strobe 2）
// ═══════════════════════════════════════════════════════════════════════════
//
// 为什么要换掉原来的实现（feifan_super3can.h 的 S3_STROBE 表）：
//
//   1) 发错总线。原来的 feifanS3Tx() 走 p4CanSendB()，而 p4CanBusB() 在
//      标准 3/Y 方案上等于 P4_CHASSIS —— 0x249 SCCM_leftStalk 在 BODY 上
//      （作者 lighting_runtime.h 里写死 out.bus = P4_BODY）。帧根本到不了
//      SCCM，所以「点了没反应」。
//   2) 触发条件写反了。原实现要求 stock 帧的 d1 高 4 位已经是 0x1x
//      （驾驶员自己先按住远光位）才开始回发；而作者的做法是**我们自己去
//      拉杆**，不依赖车上任何人先动。
//   3) 节奏不对。原实现是「等 15 帧再每帧回一条」，实际闪频被 stock 帧
//      周期（约 50 ms）绑死，而且一帧开一帧关不是 70 ms 方波。
//
// 作者的做法（lighting_reference_pure.h: LightingHs2*，v3.35.6 页面对应
// highBeamStrobe 开关）：
//
//   * 一直把杆按在 PULL 位（0x249 d1 bit4），远光常亮；
//   * 每 70 ms 翻一次电平：PULL → IDLE → PULL …，灭的那 70 ms 就是「闪」；
//   * 方波不落在车身 20 ms 调度网格上，所以边沿一到立刻发帧，不等下一帧；
//   * 帧内容永远从车上真实的 0x249 帧派生：d1 高 2 位保留、低 4 位给滚动
//     计数器、d0 重算 CRC；车上一帧都没有时用全 0 基帧（与原 SUPER_3CAN
//     表等价：d2=d3=0）。
//   * 起停手势：3 s 内连拉三次启动，再拉一次停止（0x249 d1 bit7..4 == 1
//     为一次「拉」）。面板开关是授权 + 直接启动，方便上车验证。
//
// 0x249 校验模型（auto_blinker_pure.h: leftStalkChecksumPure）：
//   CRC8(poly=0x2F, init=0) over { d1 & 0xF0, d2, d3, 0x00 }，再异或
//   计数器查表 FEIFAN_249_CKSUM_CTR[ctr]。校验错的帧不拿来当模板，
//   只累加 ckBad，方便上车时判断是不是模型不对。
//
// 总线：只走 P4_BODY（标准方案 = CAN A；YL 方案 = CAN B），与作者一致。
// ═══════════════════════════════════════════════════════════════════════════

#include <Preferences.h>

#define FEIFAN_STROBE_ID 0x249u
#define FEIFAN_STROBE_HALF_MS 70u       // 半周期：亮 70 ms / 灭 70 ms（约 7 Hz）
#define FEIFAN_STROBE_WINDOW_MS 3000u   // 三次拉杆必须落在这个窗口内
#define FEIFAN_STROBE_PULLS 3u          // 启动需要的拉杆次数
#define FEIFAN_STROBE_EDGE_GUARD_MS 40u // 同一次拉杆的抖动去重
#define FEIFAN_STROBE_CANCEL_GUARD_MS 600u  // 启动手势未落定前不认「停止」
#define FEIFAN_STROBE_TPL_FRESH_MS 300u // 模板过期就不发（车睡了 / 总线断了）
#define FEIFAN_STROBE_SYNTH_MS 1500u    // 打开开关多久没模板就改用全 0 基帧
#define FEIFAN_STROBE_HOLD_TX_MS 100u   // 强制远光：常拉电平的重发周期
// 爆闪最长连闪 30 s 自动停：页面关掉、人走开时不能让它一直闪下去。
// 强制远光不设上限（它是常亮模式，不是闪烁）。
#define FEIFAN_STROBE_MAX_MS 30000u

static const uint8_t FEIFAN_249_CKSUM_CTR[16] = {
  0x9B, 0xE8, 0x2A, 0xD3, 0xD3, 0x83, 0x4C, 0x5E,
  0x3F, 0x5E, 0xE2, 0x28, 0x3A, 0x13, 0xAF, 0xCE
};

static inline uint8_t feifan249Crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x2F) : (uint8_t)(crc << 1);
  }
  return crc;
}

// frame 至少 4 字节：只用 d1 的高 4 位 + d2 + d3，低 4 位是计数器，不进 CRC。
static inline uint8_t feifan249Checksum(const uint8_t frame[4], uint8_t counter) {
  const uint8_t crcInput[4] = {(uint8_t)(frame[1] & 0xF0), frame[2], frame[3], 0x00};
  return (uint8_t)(feifan249Crc8(crcInput, 4) ^ FEIFAN_249_CKSUM_CTR[counter & 0x0F]);
}

// ── 运行时状态 ────────────────────────────────────────────────────────────
static volatile bool fsAuth = false;   // 面板开关：授权（本行车会话有效，不写 NVS）
static volatile bool fsHold = false;   // 强制远光：常拉电平，不闪
static volatile bool fsArmed = false;  // 频闪运行中
static uint32_t fsArmedAtMs = 0;       // 手势启动时刻（0 = 面板直接启动，可立即取消）
static uint32_t fsRunStartMs = 0;      // 本轮开始闪的时刻（30 s 上限用）
static uint32_t fsLevelMs = 0;         // 当前半周期起点
static bool fsLevelPull = true;        // 当前半周期电平：true = PULL（亮）
static uint32_t fsAuthMs = 0;          // 开关打开时刻（判合成基帧用）

// 0x249 模板（来自车上真实帧）
static uint8_t fsTpl[4] = {0, 0, 0, 0};
static volatile bool fsTplValid = false;
static uint32_t fsTplMs = 0;
static uint8_t fsCounter = 0;          // 最近一次「对齐」的滚动计数器
static uint32_t fsLastTxMs = 0;

// 手势
static uint8_t fsPrevDir = 0;
static uint32_t fsPrevPullMs = 0;
static uint8_t fsPulls = 0;
static uint32_t fsWindowMs = 0;

// 诊断
static volatile uint32_t fsRx = 0, fsCkBad = 0, fsPullEdges = 0;
static volatile uint32_t fsTxOk = 0, fsTxFail = 0, fsFrames = 0;
static volatile uint8_t fsLastDir = 0;
static volatile uint8_t fsLastByte1 = 0;
static volatile bool fsSynthetic = false;   // 当前用的是合成基帧（车上没 0x249）

// ── 内部工具 ──────────────────────────────────────────────────────────────
static inline uint32_t fsNow() { return (uint32_t)millis(); }

static void fsDisarm() {
  fsArmed = false;
  fsArmedAtMs = 0;
  fsPulls = 0;
}

// 从「亮」开始，符合作者「第一帧一定是 PULL」的约定。
static void fsArm(uint32_t now, bool fromGesture) {
  fsArmed = true;
  fsLevelPull = true;
  fsLevelMs = now ? now : 1u;
  fsArmedAtMs = fromGesture ? now : 0u;
  fsRunStartMs = now ? now : 1u;
  fsPulls = 0;
}

// 0x249 只在 BODY 上；标准方案 BODY = CAN A，YL 方案 BODY = CAN B。
// 与作者 lightingService() 的 transport 判据一致：管理员挂起 / USB 被动 /
// 未全跑 / epoch 为 0 / BODY 恰好是 B 路且非召唤准入窗口 —— 都不发。
static bool fsTransportOpen() {
  if (canTxAdministrativeHold || canUsbPassive()) return false;
  if (!p4CanAllRunning()) return false;
  if (canTxEpochSnapshot() == 0) return false;
  if (P4_BODY == p4CanBusB() && !twaiNonSummonAdmissionOpen()) return false;
  return true;
}

// 组装并发送一帧 0x249。level=true → PULL 位（0x10），false → IDLE。
static bool fsSendLevel(bool pull) {
  const uint32_t now = fsNow();
  CanFrame out = {};
  out.bus = P4_BODY;
  out.can_id = FEIFAN_STROBE_ID;
  out.can_dlc = 4;
  out.data[0] = fsTpl[0];
  out.data[1] = fsTpl[1];
  out.data[2] = fsTpl[2];
  out.data[3] = fsTpl[3];
  const uint8_t ctr = (uint8_t)((fsCounter + 1u) & 0x0Fu);
  // d1 高 2 位保留（车上真实编码），低 4 位 = 计数器，bit4 = PULL。
  out.data[1] = (uint8_t)((fsTpl[1] & 0xC0u) | (pull ? 0x10u : 0x00u) | ctr);
  out.data[0] = feifan249Checksum(out.data, ctr);
  if (p4CanSendFrame(&out) == ESP_OK) {
    fsCounter = ctr;
    fsLastTxMs = now;
    fsFrames = (uint32_t)(fsFrames + 1u);
    fsTxOk = (uint32_t)(fsTxOk + 1u);
    return true;
  }
  fsTxFail = (uint32_t)(fsTxFail + 1u);
  return false;
}

// ── 开关 ──────────────────────────────────────────────────────────────────
// 爆闪开关：打开即启动（不必等拨杆），关掉立即停手。
static bool feifanStrobeSetAuth(bool on) {
  fsAuth = on;
  if (on) {
    fsAuthMs = fsNow();
    // 车上还没发过 0x249 时先用合成基帧顶上，第一次收到真帧再换。
    if (!fsTplValid) {
      fsTpl[0] = fsTpl[1] = fsTpl[2] = fsTpl[3] = 0;
      fsSynthetic = true;
    }
    fsArm(fsNow(), false);
  } else {
    fsDisarm();
  }
  return true;
}

// 强制远光：只拉不闪。与爆闪共用同一条 0x249 通道，优先级低于爆闪。
static bool feifanStrobeSetHold(bool on) {
  fsHold = on;
  if (on) {
    fsAuthMs = fsNow();
    if (!fsTplValid) {
      fsTpl[0] = fsTpl[1] = fsTpl[2] = fsTpl[3] = 0;
      fsSynthetic = true;
    }
  }
  return true;
}

// 自动转向灯也在写 0x249。爆闪占着通道时让它先停，避免两条计数器流交错
// （作者注释里「所有假爆闪报告」都是两个写者同时跑造成的）。
static inline bool feifanStrobeOwns249() { return fsArmed; }

// ── 观察：BODY 路上的 0x249 ───────────────────────────────────────────────
static void feifanStrobeObserve(uint16_t id, const uint8_t *d, uint8_t n) {
  if (id != FEIFAN_STROBE_ID || !d || n != 4) return;
  const uint32_t now = fsNow();
  fsRx = (uint32_t)(fsRx + 1u);
  fsLastByte1 = d[1];

  const uint8_t ctr = (uint8_t)(d[1] & 0x0Fu);
  const bool ckOk = (d[0] == feifan249Checksum(d, ctr));
  if (ckOk) {
    fsTpl[0] = d[0];
    fsTpl[1] = d[1];
    fsTpl[2] = d[2];
    fsTpl[3] = d[3];
    fsTplValid = true;
    fsTplMs = now;
    fsSynthetic = false;
    // 正在闪的时候别被车自己的计数器拽走，否则回发帧的计数器会来回跳。
    if (!fsArmed || !fsLastTxMs || (uint32_t)(now - fsLastTxMs) > 100u) fsCounter = ctr;
  } else {
    fsCkBad = (uint32_t)(fsCkBad + 1u);
  }

  // 杆位：d1 bit7..4，0=IDLE 1=PULL 2=PUSH 3=SNA
  const uint8_t dir = (uint8_t)((d[1] >> 4) & 0x03u);
  fsLastDir = dir;
  const bool pullEdge = (dir == 1u) && (fsPrevDir != 1u);
  fsPrevDir = dir;
  if (!pullEdge) return;
  if (fsPrevPullMs && (uint32_t)(now - fsPrevPullMs) < FEIFAN_STROBE_EDGE_GUARD_MS) return;
  fsPrevPullMs = now;
  fsPullEdges = (uint32_t)(fsPullEdges + 1u);
  if (!fsAuth) return;  // 没授权时只统计，不动手

  if (fsArmed) {
    // 启动手势还没落定（600 ms 内）时，这一拉属于启动动作本身，不能当成停止。
    if (fsArmedAtMs && (uint32_t)(now - fsArmedAtMs) < FEIFAN_STROBE_CANCEL_GUARD_MS) {
      fsPulls = 0;
      return;
    }
    fsDisarm();
    return;
  }
  if (fsPulls == 0u || (uint32_t)(now - fsWindowMs) > FEIFAN_STROBE_WINDOW_MS) {
    fsPulls = 0;
    fsWindowMs = now;
  }
  fsPulls++;
  if (fsPulls >= FEIFAN_STROBE_PULLS) fsArm(now, true);
}

// ── 服务：每 70 ms 翻一次电平并立刻发帧 ───────────────────────────────────
// 由 can_runtime.h 的 BODY 任务每轮调用（vTaskDelay(1) 量级，边沿误差 < 2 ms）。
static void feifanStrobeService() {
  if (!fsArmed && !fsHold) return;
  const uint32_t now = fsNow();

  // 模板必须新鲜。陈旧的模板不能回放：d2/d3 里可能还带着当时的转向灯状态，
  // 照抄等于替车主一直打着转向灯。过期就直接退到全 0 基帧（只保留杆位）。
  const bool tplFresh = fsTplValid &&
                        (uint32_t)(now - fsTplMs) <= FEIFAN_STROBE_TPL_FRESH_MS;
  if (!tplFresh) {
    if (!fsAuthMs || (uint32_t)(now - fsAuthMs) < FEIFAN_STROBE_SYNTH_MS) return;
    if (fsTplValid) {
      fsTplValid = false;   // 等下一次真实帧再填回来
      fsSynthetic = true;
    }
    fsTpl[0] = fsTpl[1] = fsTpl[2] = fsTpl[3] = 0;
  }

  // 屏障锁只抢 0 等待：抢不到说明有人正在冻结 TX（OTA/epoch 切换），
  // 下一毫秒再来，不要把 BODY 的 RX 循环卡住。
  if (!canTxBarrierMutex || xSemaphoreTake(canTxBarrierMutex, 0) != pdTRUE) return;
  if (!fsTransportOpen()) {
    xSemaphoreGive(canTxBarrierMutex);
    return;
  }

  if (fsArmed && fsRunStartMs && (uint32_t)(now - fsRunStartMs) > FEIFAN_STROBE_MAX_MS) {
    // 连闪超时：灯光不能无人值守地一直闪。
    fsDisarm();
    xSemaphoreGive(canTxBarrierMutex);
    return;
  }

  if (fsArmed) {
    // 方波按墙上时钟走，服务被拖慢也不会把某一半拉长。
    if (fsLevelMs == 0u) {
      fsLevelMs = now ? now : 1u;
      fsLevelPull = true;
    } else if ((uint32_t)(now - fsLevelMs) >= FEIFAN_STROBE_HALF_MS) {
      fsLevelMs = now;
      fsLevelPull = !fsLevelPull;
    }
    fsSendLevel(fsLevelPull);
  } else if (!fsLastTxMs || (uint32_t)(now - fsLastTxMs) >= FEIFAN_STROBE_HOLD_TX_MS) {
    // 强制远光：常拉电平，周期重发，不吃方波。
    fsSendLevel(true);
  }
  xSemaphoreGive(canTxBarrierMutex);
}

// ── 状态回读 ──────────────────────────────────────────────────────────────
static String feifanStrobeConfigJson() {
  String j = "{";
  j += "\"auth\":"; j += fsAuth ? "true" : "false";
  j += ",\"armed\":"; j += fsArmed ? "true" : "false";
  j += ",\"hold\":"; j += fsHold ? "true" : "false";
  j += ",\"tpl\":"; j += fsTplValid ? "true" : "false";
  j += ",\"synth\":"; j += fsSynthetic ? "true" : "false";
  j += ",\"rx\":"; j += (unsigned long)fsRx;
  j += ",\"ckBad\":"; j += (unsigned long)fsCkBad;
  j += ",\"ctr\":"; j += String((int)fsCounter);
  j += ",\"dir\":"; j += String((int)fsLastDir);
  j += ",\"byte1\":"; j += String((int)fsLastByte1);
  j += ",\"pulls\":"; j += (unsigned long)fsPullEdges;
  j += ",\"frames\":"; j += (unsigned long)fsFrames;
  j += ",\"txOk\":"; j += (unsigned long)fsTxOk;
  j += ",\"txFail\":"; j += (unsigned long)fsTxFail;
  j += "}";
  return j;
}

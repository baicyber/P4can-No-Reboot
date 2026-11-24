#pragma once
#include <stdint.h>

// ═══════════════════════════════════════════════════════════════
// SUPER_3CAN PORT — 从开源工程 SUPER_3CAN（STM32F103 + 2×MCP2515）
// 搬过来的车端 CAN 帧覆写能力。
//
// 原工程的做法：读一条真实 stock 帧 → 只改几个字节 → 用该帧自带的
// 计数器 index 查预计算表取校验字节 → 立刻回发。所以它不需要逆推
// 校验算法，16 个 index 的 payload 已经全部枚举在表里。
//
// 本文件只搬「数据表 + 触发条件」，发送走本固件 CAN B
// （chassis / VH，三路 CAN 里 Model Y 的底盘总线）。
//
// 例外：爆闪 / 强制远光（q1a、q1b）原本也在这里，但它们要发的是 BODY 路的
// 0x249 SCCM_leftStalk —— 走 B 路根本到不了 SCCM，所以已整体搬到
// feifan_strobe.h，按作者 v3.35.6 的算法重写。本文件不再碰 0x249。
//
// 安全约束：
//   1) 七项开关默认全关，只在页面显式打开后生效。
//   2) 一律「收到 stock 帧才发」，不主动造帧；无车无总线时不会乱发。
//   3) 换挡额外要求速度 ≤ 8 km/h，且只在 P/R/D 之间按原工程规则跳。
//   4) 所有发送失败只计数，不阻断 CAN 任务。
// ═══════════════════════════════════════════════════════════════

enum FfS3Kind : uint8_t {
  FF_S3_NONE = 0,
  FF_S3_STROBE,    // q1a 爆闪大灯      转 feifan_strobe.h（BODY 0x249，70 ms 方波）
  FF_S3_HIGHBEAM,  // q1b 强制远光      转 feifan_strobe.h（BODY 0x249，常拉 PULL）
  FF_S3_SERVICE,   // q1c SERVICE 模式  0x339 一次性
  FF_S3_OFFROAD,   // q1d 越野模式      0x2B6 60 00 00
  FF_S3_ESPOFF,    // q1e 关闭 ESP/漂移 0x2B6 46 00 00
  FF_S3_BOOST,     // q1f 动力狂暴      0x334 byte0 = 0xFF
  FF_S3_GEAR       // q1g 滚轮换挡      0x229 + S3_GEAR_*[]
};

static const char *FEIFAN_S3_IDS[] = {
    "q1a", "q1b", "q1c", "q1d", "q1e", "q1f", "q1g"};
static const uint8_t FEIFAN_S3_COUNT = 7;

static uint8_t feifanS3KindFor(const char *id) {
  if (!id) return FF_S3_NONE;
  for (uint8_t i = 0; i < FEIFAN_S3_COUNT; i++)
    if (strcmp(id, FEIFAN_S3_IDS[i]) == 0) return (uint8_t)(FF_S3_STROBE + i);
  return FF_S3_NONE;
}

// ── 0x229 SCCM_gearControl：按 stock 计数器 index 查表 ──────────
// 原工程 can.c 的 CAN_R / CAN_N / CAN_DD / CAN_229 / CAN_229_00，
// 每组 16 项，byte1 低 4 位是计数器、高 4 位是档位码，byte0 是校验。
static const uint8_t S3_GEAR_R[16][3] = {
    {0xAB, 0x21, 0x00}, {0xBD, 0x22, 0x00}, {0x82, 0x23, 0x00}, {0xAC, 0x24, 0x00},
    {0xAE, 0x25, 0x00}, {0x32, 0x26, 0x00}, {0x16, 0x27, 0x00}, {0xA3, 0x28, 0x00},
    {0x4A, 0x29, 0x00}, {0x19, 0x2A, 0x00}, {0x63, 0x2B, 0x00}, {0xA6, 0x2C, 0x00},
    {0xC0, 0x2D, 0x00}, {0xDE, 0x2E, 0x00}, {0xD4, 0x2F, 0x00}, {0xA9, 0x20, 0x00}};

static const uint8_t S3_GEAR_N[16][3] = {
    {0xA4, 0x11, 0x00}, {0xB2, 0x12, 0x00}, {0x8D, 0x13, 0x00}, {0xA3, 0x14, 0x00},
    {0xA1, 0x15, 0x00}, {0x3D, 0x16, 0x00}, {0x19, 0x17, 0x00}, {0xAC, 0x18, 0x00},
    {0x45, 0x19, 0x00}, {0x16, 0x1A, 0x00}, {0x6C, 0x1B, 0x00}, {0xA9, 0x1C, 0x00},
    {0xCF, 0x1D, 0x00}, {0xD1, 0x1E, 0x00}, {0xDB, 0x1F, 0x00}, {0xA6, 0x10, 0x00}};

static const uint8_t S3_GEAR_D[16][3] = {
    {0xB5, 0x41, 0x00}, {0xA3, 0x42, 0x00}, {0x9C, 0x43, 0x00}, {0xB2, 0x44, 0x00},
    {0xB0, 0x45, 0x00}, {0x2C, 0x46, 0x00}, {0x08, 0x47, 0x00}, {0xBD, 0x48, 0x00},
    {0x54, 0x49, 0x00}, {0x07, 0x4A, 0x00}, {0x7D, 0x4B, 0x00}, {0xB8, 0x4C, 0x00},
    {0xDE, 0x4D, 0x00}, {0xC0, 0x4E, 0x00}, {0xCA, 0x4F, 0x00}, {0xB7, 0x40, 0x00}};

static const uint8_t S3_GEAR_P[16][3] = {
    {0x4B, 0x31, 0x00}, {0x5D, 0x32, 0x00}, {0x62, 0x33, 0x00}, {0x4C, 0x34, 0x00},
    {0x4E, 0x35, 0x00}, {0xD2, 0x36, 0x00}, {0xF6, 0x37, 0x00}, {0x43, 0x38, 0x00},
    {0xAA, 0x39, 0x00}, {0xF9, 0x3A, 0x00}, {0x83, 0x3B, 0x00}, {0x46, 0x3C, 0x00},
    {0x20, 0x3D, 0x00}, {0x3E, 0x3E, 0x00}, {0x34, 0x3F, 0x00}, {0x49, 0x30, 0x00}};

// ── 运行时状态 ─────────────────────────────────────────────────
static volatile bool s3OffroadOn = false;
static volatile bool s3EspOffOn = false;
static volatile bool s3BoostOn = false;
static volatile bool s3GearOn = false;

static volatile uint8_t s3BurstCount = 0;    // 0x2B6 已发轮次（原工程 CANESP，上限 3）
static volatile uint8_t s3GearIdx = 0;       // 最近一条 0x229 的计数器
static volatile uint8_t s3GearState = 0;     // 1=P 2=R 3=N 4=D（来自 0x118）
static volatile uint8_t s3GearReq = 0;       // 1=上拨(要 R) 2=下拨(要 D)
static volatile uint8_t s3SpeedKph = 0xFF;   // 0x257 byte3，原工程用 <=8 作安全门限
static volatile uint32_t s3TxOk = 0, s3TxFail = 0;
static volatile uint32_t s3LastGearTxMs = 0;

// 爆闪 / 强制远光已迁到 feifan_strobe.h（走 BODY 路，作者 v3.35.6 算法），
// 这里只剩真正读「B 路 stock 帧」的几项。
static bool feifanS3AnyOn() {
  return s3OffroadOn || s3EspOffOn || s3BoostOn || s3GearOn;
}

static bool feifanS3Snap(uint8_t kind) {
  switch (kind) {
  case FF_S3_STROBE: return ffLightStrobe;
  case FF_S3_HIGHBEAM: return ffLightAp;
  case FF_S3_OFFROAD: return s3OffroadOn;
  case FF_S3_ESPOFF: return s3EspOffOn;
  case FF_S3_BOOST: return s3BoostOn;
  case FF_S3_GEAR: return s3GearOn;
  default: return false;
  }
}

static bool feifanS3Tx(uint16_t id, const uint8_t *data, uint8_t len, uint8_t repeat) {
  if (!data || len == 0 || len > 8) return false;
  twai_message_t m = {};
  m.identifier = id;
  m.data_length_code = len;
  m.flags = 0;
  for (uint8_t i = 0; i < len; i++) m.data[i] = data[i];
  bool ok = false;
  for (uint8_t i = 0; i < repeat; i++) {
    if (p4CanSendB(&m) == ESP_OK) {
      ok = true;
      s3TxOk++;
    } else {
      s3TxFail++;
    }
  }
  return ok;
}

// ── 0x339 SERVICE：一次性动作，无需 stock 帧 ────────────────────
// 原工程 main.c 是连发 3 次 {00 00 00 00 00 E0 00 00}；官方 v3.35.0 只发一帧
// data[5] = 0x80（且走 BODY 路、过 TX 屏障与准入判据）。现在统一走官方实现，
// 见 feifan_service.h 的 feifanServiceEnter()。
static bool feifanS3ServicePulse() {
  return feifanServiceEnter() == ESP_OK;
}

// ── 开关写入 ───────────────────────────────────────────────────
static bool feifanS3Set(uint8_t kind, bool on, const char *&note) {
  note = "applied";
  switch (kind) {
  case FF_S3_STROBE:
    return ffLightingSetStrobe(on);
  case FF_S3_HIGHBEAM:
    return ffLightingSetHighBeam(on);
  case FF_S3_OFFROAD:
  case FF_S3_ESPOFF:
    // 两种模式共用 0x2B6，互斥（原工程 ESP 变量只能取一个值）
    if (on) {
      if (kind == FF_S3_OFFROAD) {
        s3OffroadOn = true;
        s3EspOffOn = false;
      } else {
        s3EspOffOn = true;
        s3OffroadOn = false;
      }
      s3BurstCount = 0;
    } else {
      if (kind == FF_S3_OFFROAD) s3OffroadOn = false;
      else s3EspOffOn = false;
    }
    return true;
  case FF_S3_BOOST:
    s3BoostOn = on;
    return true;
  case FF_S3_GEAR:
    s3GearOn = on;
    s3GearReq = 0;
    if (!on) note = "applied";
    return true;
  default:
    return false;
  }
}

// ── stock 帧观察与覆写 ─────────────────────────────────────────
// 由 can_runtime.h 的 CAN B 分发表调用；只有开关打开时才产生 TX。
static void feifanS3Observe(uint16_t id, const uint8_t *d, uint8_t n) {
  if (!d || n == 0) return;
  // 全关时零开销退出：不记录、不发帧。
  if (!feifanS3AnyOn()) return;
  switch (id) {
  // 0x2B6 ESP_status：原工程收帧后按 100 ms 窗口发 2 帧、共 3 轮
  case 0x2B6: {
    if (n < 3 || s3BurstCount >= 3) return;
    const bool offroad = s3OffroadOn;
    const bool espOff = s3EspOffOn;
    if (!offroad && !espOff) return;
    static const uint8_t kEspOff[3] = {0x46, 0x00, 0x00};
    static const uint8_t kOffroad[3] = {0x60, 0x00, 0x00};
    if (feifanS3Tx(0x2B6, offroad ? kOffroad : kEspOff, 3, 2)) s3BurstCount++;
    return;
  }

  // 0x334 UI_powertrainControl：狂暴 = 功率限值拉满（原工程 byte0 = 0xFF）
  case 0x334: {
    if (!s3BoostOn || n < 8) return;
    uint8_t out[8];
    for (uint8_t i = 0; i < 8; i++) out[i] = d[i];
    out[0] = 0xFF;  // 不限功率 + 踏板最激进 + 回收回填
    out[7] = teslaAdditiveChecksumPure(0x334u, out);
    feifanS3Tx(0x334, out, 8, 1);
    return;
  }

  // 0x229 SCCM_gearControl：拿到 stock 计数器，满足条件时用对应表的 payload 覆写
  case 0x229: {
    if (n < 3) return;
    s3GearIdx = (uint8_t)(d[1] & 0x0F);
    if (!s3GearOn) return;
    const uint8_t req = s3GearReq;
    if (req == 0) return;
    if (s3SpeedKph > 8) {
      s3GearReq = 0;
      return;
    }
    const uint32_t now = (uint32_t)millis();
    if (s3LastGearTxMs && (uint32_t)(now - s3LastGearTxMs) < 400u) return;
    const uint8_t *payload = nullptr;
    if (s3GearState == 1 && req == 1) payload = S3_GEAR_R[s3GearIdx];       // P 上拨 → R
    else if (s3GearState == 1 && req == 2) payload = S3_GEAR_D[s3GearIdx];  // P 下拨 → D
    else if (s3GearState == 4 && req == 1) payload = S3_GEAR_R[s3GearIdx];  // D 上拨 → R
    else if (s3GearState == 2) payload = S3_GEAR_D[s3GearIdx];              // R → D
    else if (s3GearState == 3) payload = S3_GEAR_D[s3GearIdx];              // N → D
    s3GearReq = 0;
    if (payload && feifanS3Tx(0x229, payload, 3, 1)) s3LastGearTxMs = now;
    return;
  }

  // 0x118 ID118GearPTO：挡位 bit21~23，原工程 gear_flag 1=P 2=R 3=N 4=D
  case 0x118: {
    if (n < 3) return;
    const uint8_t raw = (uint8_t)((d[2] >> 5) & 0x07);
    s3GearState = (raw >= 1 && raw <= 4) ? raw : 0;
    return;
  }

  // 0x257 ID257VCFRONT_status：byte3 车速，原工程以 <= 8 作为换挡安全门限
  case 0x257: {
    if (n < 4) return;
    s3SpeedKph = d[3];
    return;
  }

  // 0x3C2 VCLEFT_switchStatus mux1：右滚轮滚动量（6-bit signed，byte3）
  case 0x3C2: {
    if (n < 8 || (d[0] & 0x03) != 0x01) return;
    if (!s3GearOn) return;
    uint8_t raw = d[3];
    int8_t ticks = (int8_t)raw;
    if (raw & 0x20) ticks = (int8_t)(raw - 64);
    if (ticks > 0) s3GearReq = 1;       // 上拨
    else if (ticks < 0) s3GearReq = 2;  // 下拨
    return;
  }

  default:
    return;
  }
}

// ── 状态回读（给 /api/feifan/status） ──────────────────────────
static String feifanS3ConfigJson() {
  String j = "{";
  j += "\"q1a\":"; j += ffLightStrobe ? "true" : "false";
  j += ",\"q1b\":"; j += ffLightAp ? "true" : "false";
  j += ",\"q1c\":false";
  j += ",\"q1d\":"; j += s3OffroadOn ? "true" : "false";
  j += ",\"q1e\":"; j += s3EspOffOn ? "true" : "false";
  j += ",\"q1f\":"; j += s3BoostOn ? "true" : "false";
  j += ",\"q1g\":"; j += s3GearOn ? "true" : "false";
  j += ",\"gearState\":"; j += String((int)s3GearState);
  j += ",\"speedKph\":"; j += String((int)s3SpeedKph);
  // 旧爆闪诊断位已随 feifan_strobe.h 移除；新灯光整页看 status.lighting /
  // /api/lighting/stats。这里保留同名字段避免老前端读取断裂，值指向新模块。
  j += ",\"strobeArm\":"; j += ffLightStrobe ? "true" : "false";
  j += ",\"stalkByte\":0";
  j += ",\"stalkRx\":0";
  j += ",\"txOk\":"; j += String((unsigned long)s3TxOk);
  j += ",\"txFail\":"; j += String((unsigned long)s3TxFail);
  j += "}";
  return j;
}

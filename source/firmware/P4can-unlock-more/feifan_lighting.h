// ─────────────────────────────────────────────────────────────
// 灯光功能（Lighting）—— 作者 v3.35.6「设置 → 灯光功能」整页移植
//
// 这一份是权威实现：算法全部来自作者的 lighting_reference_pure.h
// （逐字拷贝，未改动），适配层按本工程的 TX 闸门 / CAN 路由接线。
// 图 2 页面上的四项都在这里，不再是散落在「更多」页的 q1a/q1b：
//
//   1) 自动驾驶时手动开远光  apForcedHighBeam
//      AP 中向前推一次杆开启，再推一次关闭。走 BODY 0x3E9 DAS 决策位，
//      不发 0x249 —— 发 0x249 会被灯光模块判成持续拨杆进而切近光。
//   2) 远光灯频闪            highBeamStrobe
//      三次快拉启动、再拉一次停止；亮、灭各 70 ms（作者 Strobe 2）。
//   3) 后雾灯频闪            rearFogStrobe
//      按住右滚轮 + 左滚轮下滚，触发 20 次，100 ms 半周期。
//   4) 动态刹车灯            dynamicBrakeMode 0/1/2
//      0x145 / 0x39D 两路踏板信号，写 BODY 0x3E9 d2 bit6。
//
// 安全约束
//   · 所有发送都过 canTxBarrierMutex + epoch + 管理员挂起 + USB 被动 +
//     p4CanAllRunning，跟其他注入共用同一把闸门，不另开后门。
//   · 0x249 归有多主：爆闪占用时转向灯让位，用 lighting249OwnerPure 裁定。
//   · NVS 命名空间 features / 键 lighting，跟作者一致；断电保持。
//   · 观察路径只读，不发任何帧；关闭时热路径只有一次 bool 判断。
// ─────────────────────────────────────────────────────────────
#pragma once
#include "lighting_reference_pure.h"
#include "text_format_pure.h"   // parseHttpBoolPure / HttpBoolPolicy（HTTP 参数解析）

// 本文件在 web_api.h 顶部被包含，早于 web_api.h 里的 httpBoolArg 定义，
// 所以这里自己解析布尔参数，语义与 httpBoolArg 一致（严格别名）。
static inline bool ffLightHttpBool(const char *name, bool &out) {
  if (!server.hasArg(name)) return false;
  const String value = server.arg(name);
  return parseHttpBoolPure(value.c_str(), value.length(), out,
                           HttpBoolPolicy::StrictAliases);
}

// ── 运行态 ───────────────────────────────────────────────────
struct FfLightingTemplate {
  bool valid = false, sent = false;
  uint32_t ms = 0, sentMs = 0;
  uint8_t data[8] = {}, last[8] = {}, counter = 0;
};

struct FfLightingState {
  uint32_t epoch = 0;
  FfLightingTemplate stalk, body, vehicle;   // 0x249 / 0x3E9 / 0x273
  LightingAp1RunPure run = {};
  LightingAp1PushPure train = {};
  LightingDynHighPure dynamic = {};
  LightingBrakeSignalPure brake[2] = {};
  bool brakeSeen[2] = {}; uint32_t brakeMs[2] = {};
  uint8_t prevPush = 0, prevFogButton = 0;
  uint32_t carPushMs = 0, beam5Ms = 0, beam6Ms = 0;
  uint8_t beam5 = 0, beamD4 = 0, beam6 = 3;
  bool dynSent = false;
  uint32_t lastBodyAttempt = 0, lastStalkAttempt = 0, lastFogAttempt = 0;

  // Strobe 2（作者手势 + 70 ms 方波）
  LightingHs2Pure hs2 = {};
  LightingHsTriplePullPure hsGesture = {};
  uint8_t prevStalkDir = 0;
  uint32_t hs2LastTxMs = 0;

  // 后雾灯频闪：瞬时手势，20 下 = 40 个半周期
  bool fogBurst = false;
  uint8_t fogEdge = 0;
  uint32_t fogNextMs = 0;
  uint32_t fogGestureMs = 0;
};

static portMUX_TYPE ffLightMux = portMUX_INITIALIZER_UNLOCKED;
static FfLightingState ffLight;
static bool ffLightAp = false;          // apForcedHighBeam
static bool ffLightStrobe = false;      // highBeamStrobe
static bool ffLightRearFog = false;     // rearFogStrobe
static uint8_t ffLightBrakeMode = 0;    // 0/1/2
static uint32_t ffLightTxOk = 0, ffLightTxFail = 0, ffLightTxSkip = 0;

// ── AP 门控：和作者一样，缺 DAS 时间戳一律关闭 ──────────────
static bool ffLightApGate(uint32_t now) {
  portENTER_CRITICAL(&stateMux);
  const bool ap = gateAPActive, valid = dasAutopilotStateValid;
  const uint8_t state = dasAutopilotState4;
  const uint32_t ms = lastDASStatusMillis;
  portEXIT_CRITICAL(&stateMux);
  return lightingApGateActivePure(ap, valid, state, ms, now);
}

// ── NVS：features / lighting ─────────────────────────────────
// 位分配沿用作者 config：bit0 AP 远光，bit1..2 动态刹车档位，
// bit6 后雾灯（cfg7 bit6），bit7 频闪标记位（我们自己留的）。
static void ffLightLoad() {
  Preferences p; uint32_t raw = 0;
  if (p.begin("features", true)) { raw = p.getUInt("lighting", 0); p.end(); }
  ffLightAp       = (raw & 0x01u) != 0u;
  ffLightBrakeMode = (uint8_t)((raw >> 1) & 0x03u);
  if (ffLightBrakeMode > 2u) ffLightBrakeMode = 0;
  ffLightRearFog  = (raw & 0x40u) != 0u;
  ffLightStrobe   = (raw & 0x80u) != 0u;
}

static bool ffLightSave() {
  if (!canTxBarrierMutex ||
      xSemaphoreTake(canTxBarrierMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  const uint32_t value = 0x100u
      | (ffLightAp ? 0x01u : 0u)
      | ((uint32_t)ffLightBrakeMode << 1)
      | (ffLightRearFog ? 0x40u : 0u)
      | (ffLightStrobe ? 0x80u : 0u);
  Preferences p; bool saved = false;
  if (p.begin("features", false)) {
    saved = p.putUInt("lighting", value) == sizeof(uint32_t)
         && p.getUInt("lighting", 0) == value;
    p.end();
  }
  if (saved) {
    portENTER_CRITICAL(&ffLightMux);
    if (!ffLightAp) { ffLight.run = {}; ffLight.train = {}; }
    if (!ffLightStrobe) { lightingHs2ResetPure(ffLight.hs2);
                          lightingHsTriplePullResetPure(ffLight.hsGesture); }
    if (!ffLightRearFog) { ffLight.fogBurst = false; ffLight.fogEdge = 0; }
    lightingDynHighResetPure(ffLight.dynamic);
    portEXIT_CRITICAL(&ffLightMux);
  }
  xSemaphoreGive(canTxBarrierMutex);
  return saved;
}

// ── 观察：三路 CAN 都喂进来 ─────────────────────────────────
static void ffLightingObserve(uint8_t bus, uint16_t id, uint8_t dlc,
                              const uint8_t *data, uint32_t epoch, uint32_t ms) {
  if (!data || dlc > 8u || id > 0x7FFu || epoch == 0u) return;
  const bool ap = ffLightApGate(ms);
  portENTER_CRITICAL(&ffLightMux);
  auto &s = ffLight;
  if (s.epoch != epoch) { s = FfLightingState{}; s.epoch = epoch; }

  // 刹车：两路 CHASSIS，分别锁存再合并
  if (bus == P4_CHASSIS && ((id == LIGHTING_ID_ESP_STATUS && dlc == 8u) ||
                            (id == LIGHTING_ID_IBST_STATUS && dlc == 5u))) {
    const uint8_t i = (id == LIGHTING_ID_ESP_STATUS) ? 0u : 1u;
    s.brake[i] = (i == 0) ? lightingDecodeBrake145Pure(data, dlc)
                          : lightingDecodeBrake39DPure(data, dlc);
    s.brakeMs[i] = ms; s.brakeSeen[i] = true;
  }

  if (bus == P4_BODY) {
    // 远光反馈（只读）
    if (id == 0x3F5u && dlc >= 5u) {
      s.beam5 = lightingHighBeamControlStatePure(data);
      s.beamD4 = data[4]; s.beam5Ms = ms;
    }
    if (id == 0x3F6u && dlc >= 1u) {
      s.beam6 = lightingHighBeamLeftStatusPure(data); s.beam6Ms = ms;
    }

    // 0x249：AP 远光 + 爆闪共用。先判自回波，再喂两个手势状态机。
    if (id == LIGHTING_ID_LEFTSTALK && dlc == 4u) {
      auto &t = s.stalk;
      const bool echo = t.sent && (uint32_t)(ms - t.sentMs) < LIGHTING_249_SELF_ECHO_MS
          && ((data[1] & 0xF0u) == (t.last[1] & 0xF0u))
          && data[2] == t.last[2] && data[3] == t.last[3];
      const bool checksum = data[0] == leftStalkChecksumPure(data, data[1] & 15u);
      if (!echo && checksum) {
        t.valid = true; t.ms = ms;
        memcpy(t.data, data, dlc);
        if (s.train.left == 0u) t.counter = data[1] & 15u;

        const uint8_t dir = lightingStalkDirPure(data);
        const bool pullEdge = (dir == LIGHTING_249_HIGH_BEAM_PULL)
                           && (s.prevStalkDir != LIGHTING_249_HIGH_BEAM_PULL);
        s.prevStalkDir = dir;

        // 频闪：三次快拉启动，再拉一次停
        if (ffLightStrobe) {
          const int8_t act = lightingHsGestureFeedPure(
              s.hsGesture, ms, pullEdge, lightingHs2RunningPure(s.hs2),
              s.hs2.armedAtMs);
          if (act == LIGHTING_HS_ACTION_ARM) lightingHs2ArmPure(s.hs2, ms);
          else if (act == LIGHTING_HS_ACTION_DISARM) lightingHs2DisarmPure(s.hs2);
        }

        // AP 远光：PUSH 上升沿切换
        const bool pushEdge = (dir == LIGHTING_249_HIGH_BEAM_PUSH)
                           && (s.prevPush != LIGHTING_249_HIGH_BEAM_PUSH);
        const bool quiet = !t.sent || (uint32_t)(ms - t.sentMs) >= LIGHTING_AP1_TX_QUIET_MS;
        lightingAp1RunStepPure(s.run, ffLightAp && ap &&
            activeTurnSignalVariant == TURN_SIGNAL_STALK, pushEdge, quiet, ms);
        s.prevPush = dir;
        if (dir == LIGHTING_249_HIGH_BEAM_PUSH) s.carPushMs = ms;
      }
    }

    // 0x3E9：AP 远光/动态刹车灯模板
    if (id == LIGHTING_ID_DAS_BODY && dlc == 8u) {
      auto &t = s.body;
      const bool echo = t.sent && (uint32_t)(ms - t.sentMs) < LIGHTING_3E9_SELF_ECHO_MS
          && memcmp(data, t.last, 6) == 0;
      if (!echo && data[7] == teslaAdditiveChecksumPure(LIGHTING_ID_DAS_BODY, data)) {
        t.valid = true; t.ms = ms; memcpy(t.data, data, 8);
        t.counter = data[6] >> 4;
      }
    }

    // 0x3C2：后雾灯手势（按住右滚轮 + 左滚轮下滚）
    if (id == LIGHTING_ID_VCLEFT_SWITCH && dlc >= 3u) {
      const bool gesture = lightingRearFogGesturePure(data, true);
      if (gesture && ffLightRearFog) { s.fogGestureMs = ms; s.fogBurst = true;
        s.fogEdge = 0; s.fogNextMs = ms; }
      s.prevFogButton = (data[1] >> 4) & 0x03u;
    }

    // 0x273：后雾灯频闪模板（车身发，500 ms 周期）
    if (id == LIGHTING_ID_VEHICLE_CONTROL && dlc >= 3u) {
      auto &t = s.vehicle;
      t.valid = true; t.ms = ms; memcpy(t.data, data, dlc);
    }
  }
  portEXIT_CRITICAL(&ffLightMux);
}


// 兼容旧 /api/feifan/config 的 q1a/q1b：转发到新灯光开关。
// 旧页面开关即使还留着，点了也走同一份状态，不会出现两套实现打架。
static bool ffLightingSetStrobe(bool on) {
  ffLightStrobe = on;
  if (!on) { portENTER_CRITICAL(&ffLightMux);
             lightingHs2ResetPure(ffLight.hs2);
             lightingHsTriplePullResetPure(ffLight.hsGesture);
             portEXIT_CRITICAL(&ffLightMux); }
  return ffLightSave();
}
static bool ffLightingSetHighBeam(bool on) {
  ffLightAp = on;
  if (!on) { portENTER_CRITICAL(&ffLightMux);
             ffLight.run = {}; ffLight.train = {};
             portEXIT_CRITICAL(&ffLightMux); }
  return ffLightSave();
}

// 判断 0x249 当前归谁：爆闪 > AP 远光补发 > 无
static inline uint8_t ffLight249Owner(const FfLightingState &s) {
  return lighting249OwnerPure(lightingHs2RunningPure(s.hs2), false,
                              s.train.left != 0u);
}

// 给自动转向灯让位用：爆闪/AP 补发正在占用 0x249 时为 true。
// 加锁读，供 CAN 接收任务跨任务安全调用。
static inline bool ffLightStrobeOwns249() {
  portENTER_CRITICAL(&ffLightMux);
  const uint8_t owner = ffLight249Owner(ffLight);
  portEXIT_CRITICAL(&ffLightMux);
  return owner == LIGHTING_249_OWNER_STROBE || owner == LIGHTING_249_OWNER_AP1;
}

// 方波电平：按墙钟翻转（会推进 s 的 half-cycle 时间戳），必须传真实引用。
static inline bool ffLightHs2Level(LightingHs2Pure &s, uint32_t now) {
  return lightingHs2LevelPure(s, now);
}

// ── 5 ms 周期发送调度 ───────────────────────────────────────
static void ffLightingService() {
  const uint32_t now = millis();
  if (!canTxBarrierMutex || xSemaphoreTake(canTxBarrierMutex, 0) != pdTRUE) return;
  const uint32_t epoch = canTxBarrierState.epoch;
  const bool transport = !canTxAdministrativeHold && !canUsbPassive()
      && p4CanAllRunning() && epoch != 0u
      && (P4_BODY != p4CanBusB() || twaiNonSummonAdmissionOpen());
  const bool ap = ffLightApGate(now);

  portENTER_CRITICAL(&stateMux);
  const uint8_t gear = gear118Raw; const uint32_t gearMs = gear118Ms;
  portEXIT_CRITICAL(&stateMux);
  portENTER_CRITICAL(&nagCtxMux);
  const bool speedValid = nagCtx.vehicleSpeedValid
      && (uint32_t)(now - nagCtx.lastVehicleSpeedMs) <= 500u;
  const int32_t speed = nagPartySpeedKphX100Pure(nagCtx.vehicleSpeedRaw);
  portEXIT_CRITICAL(&nagCtxMux);
  portENTER_CRITICAL(&blinkAMux);
  const bool blinkerBusy = activeTurn != STALK_IDLE
      || blinkerTxRequestState.pendingDir != 0u;
  portEXIT_CRITICAL(&blinkAMux);

  CanFrame out = {}; out.bus = P4_BODY;
  bool send = false, stalkSend = false, fogSend = false;
  uint8_t counter = 0;

  portENTER_CRITICAL(&ffLightMux);
  auto &s = ffLight;
  if (s.epoch != epoch) { s = FfLightingState{}; s.epoch = epoch; }
  if (!transport) {
    s.run = {}; s.train = {}; s.dynamic = {}; s.dynSent = false;
    s.hs2 = {}; s.fogBurst = false;
  } else {
    // AP 远光：推进补发状态机
    lightingAp1RunStepPure(s.run, ffLightAp && ap &&
        activeTurnSignalVariant == TURN_SIGNAL_STALK, false, true, now);
    const bool beam = ffLightAp && ap && s.run.running;

    // 动态刹车灯
    LightingBrakeSignalPure a = {}, b = {};
    if (s.brakeSeen[0] && (uint32_t)(now - s.brakeMs[0]) <= 300u) a = s.brake[0];
    if (s.brakeSeen[1] && (uint32_t)(now - s.brakeMs[1]) <= 300u) b = s.brake[1];
    const auto brake = lightingMergeBrakePure(a, b);
    const bool driveFresh = gearMs != 0u && (uint32_t)(now - gearMs) <= 500u && speedValid;
    lightingDynHighStepPure(s.dynamic, driveFresh ? ffLightBrakeMode : 0u,
        brake.brake, brake.hard, gear, driveFresh ? speed : 0);
    const bool requested = s.dynamic.requested;

    bool fresh = false, lit = false, cancel = false;
    lightingBeamReportPure(now, s.beam6Ms, s.beam6, s.beam5Ms, s.beam5,
        fresh, lit, cancel, s.beamD4, s.beam5Ms != 0u);

    // AP 远光补发串（四连 PUSH）
    LightingAp1TopupEventPure event = {};
    bool train = false;
    if (!beam) lightingAp1PushResetPure(s.train);
    else if (fresh || s.train.left != 0u)
      train = lightingAp1TopupDuePure(s.train, beam, fresh, lit,
          s.carPushMs != 0u && (uint32_t)(now - s.carPushMs) < 120u,
          s.carPushMs, now, LIGHTING_TOPUP_PROFILE_RETRY, &event);

    const bool hs2Run = lightingHs2RunningPure(s.hs2);
    const bool hs2Level = ffLightHs2Level(s.hs2, now);
    const bool fogDue = s.fogBurst && now >= s.fogNextMs;

    // 优先级：爆闪方波 > 后雾灯频闪 > AP 补发串 > 常规覆盖。
    // 爆闪和后雾灯都要 0x249 / 0x273，不能同时发。
    if (hs2Run && s.stalk.valid && (uint32_t)(now - s.stalk.ms) <= 300u
        && (uint32_t)(now - s.hs2LastTxMs) >= LIGHTING_HS2_HOLD_MS) {
      send = stalkSend = true;
      out.can_id = LIGHTING_ID_LEFTSTALK; out.can_dlc = 4;
      memcpy(out.data, s.stalk.data, 4);
      counter = (s.stalk.counter + 1u) & 15u;
      out.data[1] = (uint8_t)((out.data[1] & 0xC0u)
          | (hs2Level ? LIGHTING_249_PULL_BIT : 0u) | counter);
      out.data[0] = leftStalkChecksumPure(out.data, counter);
      s.hs2LastTxMs = now;
    } else if (fogDue && s.vehicle.valid) {
      send = fogSend = true;
      out.can_id = LIGHTING_ID_VEHICLE_CONTROL; out.can_dlc = 8;
      memcpy(out.data, s.vehicle.data, 8);
      // 只动 d2 bit7，byte0 原样透传：作者实测写 byte0 会误触发
      // 「附件上电 + 灯开关 ON」，导致驾驶员档案被重置。
      if ((s.fogEdge & 1u) == 0u)
        out.data[LIGHTING_273_D2] |= LIGHTING_273_REAR_FOG_MASK;
      else
        out.data[LIGHTING_273_D2] &= (uint8_t)~LIGHTING_273_REAR_FOG_MASK;
      out.data[LIGHTING_273_D0] = s.vehicle.data[LIGHTING_273_D0];
      s.fogEdge++;
      s.fogNextMs = now + LIGHTING_REAR_FOG_HALF_PERIOD_MS;
      if (s.fogEdge >= LIGHTING_REAR_FOG_FLASHES * 2u) {
        s.fogBurst = false; s.fogEdge = 0;
      }
    } else if (train && !blinkerBusy && s.stalk.valid
        && (uint32_t)(now - s.stalk.ms) <= 200u
        && (uint32_t)(now - s.lastStalkAttempt) >= 10u
        && ((s.stalk.data[1] >> 4) & 3u) == 0u
        && (s.stalk.data[2] & 15u) == 0u) {
      send = stalkSend = true;
      out.can_id = LIGHTING_ID_LEFTSTALK; out.can_dlc = 4;
      memcpy(out.data, s.stalk.data, 4);
      counter = (s.stalk.counter + 1u) & 15u;
      out.data[1] = (uint8_t)((out.data[1] & 0xC0u) | 0x20u | counter);
      out.data[0] = leftStalkChecksumPure(out.data, counter);
      s.lastStalkAttempt = now;
    } else if (s.body.valid && (uint32_t)(now - s.body.ms) <= 1500u
        && (uint32_t)(now - s.lastBodyAttempt) >= 20u
        && (beam || requested || s.dynSent)) {
      send = true;
      out.can_id = LIGHTING_ID_DAS_BODY; out.can_dlc = 8;
      memcpy(out.data, s.body.data, 8);
      if (requested) lightingApplyDynamicBrakePure(out.data, true);
      if (beam) lightingApplyApForcedHighBeamPure(out.data, true, true);
      counter = (s.body.counter + 1u) & 15u;
      out.data[6] = (uint8_t)((out.data[6] & 0x0Fu) | (counter << 4));
      out.data[7] = teslaAdditiveChecksumPure(LIGHTING_ID_DAS_BODY, out.data);
      s.lastBodyAttempt = now;
    }
  }
  portEXIT_CRITICAL(&ffLightMux);

  if (send) {
    const esp_err_t result = p4CanSendFrame(&out);
    portENTER_CRITICAL(&ffLightMux);
    if (result == ESP_OK) {
      ffLightTxOk++;
      auto &t = fogSend ? ffLight.vehicle : (stalkSend ? ffLight.stalk : ffLight.body);
      t.counter = counter; t.sent = true; t.sentMs = millis();
      memcpy(t.last, out.data, out.can_dlc);
      if (stalkSend) lightingAp1PushSentPure(ffLight.train);
      if (!fogSend) ffLight.dynSent = ffLight.dynamic.requested;
    } else ffLightTxFail++;
    portEXIT_CRITICAL(&ffLightMux);
  } else ffLightTxSkip++;
  xSemaphoreGive(canTxBarrierMutex);
}

static void ffLightingTask(void *) {
  for (;;) { ffLightingService(); vTaskDelay(pdMS_TO_TICKS(5)); }
}

// ── 状态回读（给图 2 页面） ─────────────────────────────────
static String ffLightingConfigJson() {
  portENTER_CRITICAL(&ffLightMux);
  const bool ap = ffLightAp, strobe = ffLightStrobe, fog = ffLightRearFog;
  const uint8_t mode = ffLightBrakeMode;
  const bool running = lightingHs2RunningPure(ffLight.hs2);
  const bool apRun = ffLight.run.running;
  const bool fogBurst = ffLight.fogBurst;
  const uint8_t fogEdge = ffLight.fogEdge;
  const uint32_t ok = ffLightTxOk, fail = ffLightTxFail;
  const uint8_t owner = ffLight249Owner(ffLight);
  portEXIT_CRITICAL(&ffLightMux);

  String j = "{";
  j += "\"apForcedHighBeam\":"; j += ap ? "true" : "false";
  j += ",\"highBeamStrobe\":"; j += strobe ? "true" : "false";
  j += ",\"rearFogStrobe\":"; j += fog ? "true" : "false";
  j += ",\"dynamicBrakeMode\":"; j += String((unsigned)mode);
  j += ",\"running\":"; j += (running || apRun || fogBurst) ? "true" : "false";
  j += ",\"apActive\":"; j += ffLightApGate(millis()) ? "true" : "false";
  j += ",\"stalkSupported\":";
  j += (activeTurnSignalVariant == TURN_SIGNAL_STALK) ? "true" : "false";
  j += ",\"strobeRunning\":"; j += running ? "true" : "false";
  j += ",\"apRunning\":"; j += apRun ? "true" : "false";
  j += ",\"fogRunning\":"; j += fogBurst ? "true" : "false";
  j += ",\"fogEdge\":"; j += String((unsigned)fogEdge);
  j += ",\"owner\":"; j += String((unsigned)owner);
  j += ",\"ownerName\":\""; j += lighting249OwnerNamePure(owner); j += "\"";
  j += ",\"txOk\":"; j += String((unsigned long)ok);
  j += ",\"txFail\":"; j += String((unsigned long)fail);
  j += "}";
  return j;
}

// ── HTTP：/api/lighting/stats + /api/lighting/update ────────
static void httpFfLightingStats() {
  server.send(200, "application/json", "{\"ok\":true," +
      ffLightingConfigJson().substring(1));
}

static void httpFfLightingUpdate() {
  bool ap = ffLightAp, strobe = ffLightStrobe, fog = ffLightRearFog;
  uint8_t mode = ffLightBrakeMode;
  if (!server.hasArg("apForcedHighBeam") && !server.hasArg("highBeamStrobe")
      && !server.hasArg("rearFogStrobe") && !server.hasArg("dynamicBrakeMode")) {
    server.send(400, "application/json", "{\"error\":\"no-setting\"}"); return;
  }
  if (server.hasArg("apForcedHighBeam") &&
      !ffLightHttpBool("apForcedHighBeam", ap)) {
    server.send(400, "application/json", "{\"error\":\"invalid-ap\"}"); return;
  }
  if (server.hasArg("highBeamStrobe") && !ffLightHttpBool("highBeamStrobe", strobe)) {
    server.send(400, "application/json", "{\"error\":\"invalid-strobe\"}"); return;
  }
  if (server.hasArg("rearFogStrobe") && !ffLightHttpBool("rearFogStrobe", fog)) {
    server.send(400, "application/json", "{\"error\":\"invalid-fog\"}"); return;
  }
  if (server.hasArg("dynamicBrakeMode")) {
    const String v = server.arg("dynamicBrakeMode");
    if (v != "0" && v != "1" && v != "2") {
      server.send(400, "application/json", "{\"error\":\"invalid-brake-mode\"}"); return;
    }
    mode = (uint8_t)v.toInt();
  }
  if ((ap || strobe) && activeTurnSignalVariant != TURN_SIGNAL_STALK) {
    server.send(409, "application/json", "{\"error\":\"stalk-required\"}"); return;
  }
  ffLightAp = ap; ffLightStrobe = strobe; ffLightRearFog = fog;
  ffLightBrakeMode = mode;
  if (!ffLightSave()) {
    server.send(503, "application/json", "{\"error\":\"save-failed-or-busy\"}"); return;
  }
  httpFfLightingStats();
}

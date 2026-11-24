#pragma once

// ═══════════════════════════════════════════════════════════════
// 更多功能 → P4 原生设置 映射层
// ═══════════════════════════════════════════════════════════════
//
// 这一层只做"接线"：把页面开关转到 P4-CAN v3.29.3 里**已经存在**的
// 设置函数，由那些函数自己发 CAN 帧。不复制 CAN 逻辑、不新造帧、
// 不依赖任何外挂模块。没接上的项保持 stored，不假装生效。
//
// 全部依赖（vehicle_logic.h / can_core.h / continuous_ap_runtime.h /
// vehicle_profile.h / p4can_core_state.h）在主 .ino 里都先于
// web_api.h(→feifan_api.h→本文件) 被 include，符号可直接调用。
//
// 严格 1:1 才接：value 语义、gate、回显值三者都对得上。
// a2f(AP 自动恢复) 页面取值是 TSL 配置块语义(0关/1双击右滚轮/
// 2下拉档杆/3跟随AP)，与 ContApMethod(Unset/StalkDouble/ScrollSingle/
// ScrollDouble) 不是一回事，硬映射等于造假，故不在此表。

enum FfNative : uint8_t {
  FF_NATIVE_NONE = 0,
  FF_NATIVE_NAG,         // d1a / a1d  Nag Killer（方向盘提醒抑制同源）
  FF_NATIVE_ISA,         // a1b  ISA 提示音抑制
  FF_NATIVE_PEDAL,       // d1c  加速模式（会话级，不写 NVS）
  FF_NATIVE_ALC293,      // d1b  打灯变道 0x293
  FF_NATIVE_DMS,         // a1a  注视提醒抑制 / 驾驶员监测
  FF_NATIVE_VISION,      // a1c  禁用视觉限速
  FF_NATIVE_EAP,         // a3a  EAP 增强 / 自动转向灯
  FF_NATIVE_SUMMON_ON,   // a3d  召唤心跳开关（会话级）
  FF_NATIVE_SUMMON_VAL   // a3e  召唤心跳值 0..3（会话级）
};

static const char *FEIFAN_NATIVE_IDS[] = {
    "d1a", "a1b", "d1c", "d1b", "a1a",
    "a1c", "a1d", "a3a", "a3d", "a3e"};
static const uint8_t FEIFAN_NATIVE_COUNT = 10;

static uint8_t feifanNativeFor(const String &id) {
  if (id == "d1a" || id == "a1d") return FF_NATIVE_NAG;
  if (id == "a1b") return FF_NATIVE_ISA;
  if (id == "d1c") return FF_NATIVE_PEDAL;
  if (id == "d1b") return FF_NATIVE_ALC293;
  if (id == "a1a") return FF_NATIVE_DMS;
  if (id == "a1c") return FF_NATIVE_VISION;
  if (id == "a3a") return FF_NATIVE_EAP;
  if (id == "a3d") return FF_NATIVE_SUMMON_ON;
  if (id == "a3e") return FF_NATIVE_SUMMON_VAL;
  return FF_NATIVE_NONE;
}

// ── 三项本固件已有设置路径（原先在 feifan_api.h 里，收拢到本层） ──────
static bool ffNagApply(bool on) {
  NagConfig nc;
  portENTER_CRITICAL(&nagCfgMux);
  nc = nagCfg;
  portEXIT_CRITICAL(&nagCfgMux);
  nc.enabled = on;
  if (!nagCfgSaveCandidate(nc)) return false;
  nagCfgCommit(nc);
  return true;
}

static bool ffIsaApply(bool on) { return isaSuppressionControlApply(on); }

// PedalMap 是本次行车的临时覆盖（挂 P 挡或重启清除），与 LAB 里行为一致。
static bool ffPedalApply(const String &value) {
  uint8_t raw = PEDAL_MAP_RAW_STOCK;
  if (value == "comfort" || value == "chill") raw = PEDAL_MAP_RAW_CHILL;
  else if (value == "sport") raw = PEDAL_MAP_RAW_SPORT;
  else if (value == "standard" || value == "stock") raw = PEDAL_MAP_RAW_STOCK;
  else return false;
  return requestPedalMapMode(raw, "Feifan UI");
}

static bool ffAlc293Apply(bool on) {
  FeatureConfigMigrationPure next = featureConfigRuntimeSnapshot();
  next.autoLaneChange.enabled = on;
  if (!autoLaneChangeLabCfgPersistCandidate(next)) return false;
  portENTER_CRITICAL(&autoLc293Mux);
  uiAutoLaneChangeEnabled = on;
  portEXIT_CRITICAL(&autoLc293Mux);
  return true;
}

// 返回 true = 真的落到车控路径；false 时 gateErr 非空表示"当前车型/
// 模式不支持"（调用方回 409），否则是保存失败（回 502）。
static bool feifanNativeApply(uint8_t kind, const String &value,
                              const char *&note, const char *&gateErr) {
  note = "native-apply-failed";
  gateErr = nullptr;
  const bool on = (value == "1" || value == "true");

  switch (kind) {
    case FF_NATIVE_NAG:
      if (on && !activeProfileNagSupported()) { gateErr = "nag-unavailable"; return false; }
      if (!ffNagApply(on)) { note = "nag-save-failed"; return false; }
      note = "nag-enabled-updated";
      return true;

    case FF_NATIVE_ISA:
      if (on && !activeProfileIsaSuppressionSupported()) { gateErr = "isa-unavailable"; return false; }
      if (!ffIsaApply(on)) { note = "isa-apply-failed"; return false; }
      note = "isa-updated";
      return true;

    case FF_NATIVE_PEDAL:
      if (!ffPedalApply(value)) { note = "pedal-rejected"; return false; }
      note = "pedal-session-set";
      return true;

    case FF_NATIVE_ALC293:
      if (!labMenuEnabled) { gateErr = "lab-disabled"; return false; }
      if (!ffAlc293Apply(on)) { note = "alc293-persist-failed"; return false; }
      note = "alc293-updated";
      return true;

    case FF_NATIVE_DMS:
      if (on && !activeProfileDmsNagSupported()) { gateErr = "dms-unavailable"; return false; }
      if (!driverMonitoringControlApply(on)) { note = "dms-apply-failed"; return false; }
      note = "dms-updated";
      return true;

    case FF_NATIVE_VISION:
      if (on && !visionControlSupported()) { gateErr = "vision-unavailable"; return false; }
      if (!visionControlApplySelection(on)) { note = "vision-apply-failed"; return false; }
      note = "vision-updated";
      return true;

    case FF_NATIVE_EAP:
      if (on && !activeProfileAdvancedEapSupported()) { gateErr = "eap-unavailable"; return false; }
      if (!setAutoBlinkerEnabled(on, true)) { note = "eap-apply-failed"; return false; }
      note = "eap-updated";
      return true;

    case FF_NATIVE_SUMMON_ON: {
      if (on && (!labMenuEnabled || !summonHeartbeatOverrideSupported())) {
        gateErr = "summon-lab-disabled"; return false;
      }
      const uint8_t cur = summonHeartbeatOverrideValue;
      if (!summonHeartbeatOverrideApply(on, cur)) { note = "summon-apply-failed"; return false; }
      note = "summon-updated";
      return true;
    }

    case FF_NATIVE_SUMMON_VAL: {
      const long v = value.toInt();
      if (value != "0" && v <= 0) { note = "summon-bad-value"; return false; }
      if (v < 0 || v > 3) { note = "summon-bad-value"; return false; }
      const bool cur = summonHeartbeatOverrideEnabled;
      if (cur && (!labMenuEnabled || !summonHeartbeatOverrideSupported())) {
        gateErr = "summon-lab-disabled"; return false;
      }
      if (!summonHeartbeatOverrideApply(cur, (uint8_t)v)) {
        note = "summon-apply-failed"; return false;
      }
      note = "summon-value-updated";
      return true;
    }

    default:
      return false;
  }
}

// 会话级项（d1c / a3d / a3e）重启即失效，不进权威回显，避免留下
// "重进页面还亮着、其实车已经恢复默认" 的假状态。
static void ffNativePut(String &j, bool &first, const char *id, long v) {
  if (!first) j += ',';
  first = false;
  j += '"'; j += id; j += "\":\""; j += v; j += '"';
}

static String feifanNativeConfigJson() {
  String j = "{";
  bool first = true;

  bool nag = false;
  portENTER_CRITICAL(&nagCfgMux);
  nag = nagCfg.enabled;
  portEXIT_CRITICAL(&nagCfgMux);
  ffNativePut(j, first, "d1a", nag ? 1 : 0);
  ffNativePut(j, first, "a1d", nag ? 1 : 0);   // 同源：两个开关显示一致
  ffNativePut(j, first, "a1b", isaSuppressionControlSnapshot() ? 1 : 0);
  ffNativePut(j, first, "d1b", featureConfigRuntimeSnapshot().autoLaneChange.enabled ? 1 : 0);
  ffNativePut(j, first, "a1a", driverMonitoringControlSnapshot() ? 1 : 0);
  ffNativePut(j, first, "a1c", visionControlRequestDisabled() ? 1 : 0);
  ffNativePut(j, first, "a3a", summonCfgRuntimeSnapshot().blinker ? 1 : 0);

  j += '}';
  return j;
}

static String feifanNativeAppliedJson() {
  String j = "[";
  for (uint8_t i = 0; i < FEIFAN_NATIVE_COUNT; i++) {
    if (i) j += ',';
    j += '"'; j += FEIFAN_NATIVE_IDS[i]; j += '"';
  }
  j += ']';
  return j;
}

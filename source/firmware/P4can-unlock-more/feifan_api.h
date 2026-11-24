#pragma once
#include <Preferences.h>
#include "tsl_ble.h"
#include "feifan_native.h"
#include "feifan_service.h"
#include "feifan_lighting.h"   // 作者 v3.35.6 灯光整页；原 feifan_strobe.h 已并入
#include "feifan_super3can.h"
#include "feifan_eap_norestart.h"

// ═══════════════════════════════════════════════════════════════
// 更多功能（More features）后端 —— /api/feifan/*
// ═══════════════════════════════════════════════════════════════
//
// 页面发两种请求：
//   POST /api/feifan/config {id, value}
//   POST /api/feifan/action {id, code}
//
// 服务端对每一项如实回答"到底生效了没有"，三条通道按优先级尝试：
//   effect=applied  真的下发到车控：
//                   [1] P4 原生设置（本固件自己发 CAN 帧，不需要任何外挂硬件）
//                   [2] TSL 模块配置块 / 动作码（需要车上装了 TSL 模块）
//   effect=stored   没有可用通道，设置只写进 NVS，响应 pending=true
//
// 无 TSL 模块是本设备的常态：默认不拉起 BLE 扫描，模块项直接回
// reason=module-absent，页面标「需模块」。用户以后装了模块，可用
// POST /api/feifan/module {"on":true} 手动启用，行为自动恢复。
// 不做假回执。宁可返回 pending，也不让页面以为车动了。

#define FEIFAN_NVS_NS "feifan"
#define FEIFAN_CFG_KEY "cfg"
#define FEIFAN_CFG_MAX 3000
#define FEIFAN_TOKEN_ID 16
#define FEIFAN_TOKEN_VAL 24

static String feifanCfgText;
static bool feifanCfgReady = false;
static uint32_t feifanWritesOk = 0;
static uint32_t feifanWritesFail = 0;
static uint32_t feifanActionsBlocked = 0;
static char feifanLastId[FEIFAN_TOKEN_ID + 1] = "";
static char feifanLastEffect[12] = "";

// 说明：effect 判定与原生设置函数已收拢到 feifan_native.h，
// 本文件只负责 NVS 持久化、HTTP 编解码与模块链路。

// 只接受 [A-Za-z0-9._-]，长度受限。id 与 value 都会原样回到 JSON 里，
// 所以这里同时是注入防线。
static bool feifanTokenOk(const String &s, uint8_t maxLen) {
  if (s.length() == 0 || s.length() > maxLen) return false;
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static void feifanCfgEnsureLoaded() {
  if (feifanCfgReady) return;
  feifanCfgReady = true;
  Preferences p;
  if (!p.begin(FEIFAN_NVS_NS, true)) return;
  feifanCfgText = p.getString(FEIFAN_CFG_KEY, "");
  p.end();
  if (feifanCfgText.length() > FEIFAN_CFG_MAX) feifanCfgText = "";
}

static bool feifanCfgLookup(const String &id, String &value) {
  const int n = feifanCfgText.length();
  int pos = 0;
  while (pos < n) {
    int eol = feifanCfgText.indexOf('\n', pos);
    if (eol < 0) eol = n;
    const int eq = feifanCfgText.indexOf('=', pos);
    if (eq > pos && eq < eol) {
      if (feifanCfgText.substring(pos, eq) == id) {
        value = feifanCfgText.substring(eq + 1, eol);
        return true;
      }
    }
    pos = eol + 1;
  }
  return false;
}

// 逐行重写后整块落 NVS。失败不污染内存副本。
static bool feifanCfgWrite(const String &id, const String &value) {
  String next;
  next.reserve(feifanCfgText.length() + id.length() + value.length() + 4);
  bool replaced = false;
  const int n = feifanCfgText.length();
  int pos = 0;
  while (pos < n) {
    int eol = feifanCfgText.indexOf('\n', pos);
    if (eol < 0) eol = n;
    const String line = feifanCfgText.substring(pos, eol);
    const int eq = line.indexOf('=');
    if (!replaced && eq > 0 && line.substring(0, eq) == id) {
      next += id;
      next += '=';
      next += value;
      next += '\n';
      replaced = true;
    } else if (line.length() > 0) {
      next += line;
      next += '\n';
    }
    pos = eol + 1;
  }
  if (!replaced) {
    next += id;
    next += '=';
    next += value;
    next += '\n';
  }
  if (next.length() > FEIFAN_CFG_MAX) return false;

  Preferences p;
  if (!p.begin(FEIFAN_NVS_NS, false)) return false;
  const size_t written = p.putString(FEIFAN_CFG_KEY, next);
  p.end();
  if (written == 0) return false;
  feifanCfgText = next;
  return true;
}

// 页面发 JSON，固件不带 ArduinoJson，这里只取两个扁平字段：
// {"id":"d1a","value":1} / {"id":"d1c","value":"sport"}
static bool feifanJsonField(const String &body, const char *key, String &out) {
  String pat = "\"";
  pat += key;
  pat += "\"";
  const int at = body.indexOf(pat);
  if (at < 0) return false;
  int i = body.indexOf(':', at + pat.length());
  if (i < 0) return false;
  int p = i + 1;
  const int n = body.length();
  while (p < n && (body[p] == ' ' || body[p] == '\t')) p++;
  if (p >= n) return false;
  if (body[p] == '"') {
    const int q = body.indexOf('"', p + 1);
    if (q < 0) return false;
    out = body.substring(p + 1, q);
    return true;
  }
  int q = p;
  while (q < n && body[q] != ',' && body[q] != '}' && body[q] != ' ' &&
         body[q] != '\n' && body[q] != '\r') {
    q++;
  }
  out = body.substring(p, q);
  out.trim();
  return out.length() > 0;
}

// effect 判定在 feifan_native.h 的 feifanNativeFor()。

static String feifanCfgJsonObject() {
  String o;
  o.reserve(feifanCfgText.length() + 8);
  o = '{';
  bool first = true;
  const int n = feifanCfgText.length();
  int pos = 0;
  while (pos < n) {
    int eol = feifanCfgText.indexOf('\n', pos);
    if (eol < 0) eol = n;
    const String line = feifanCfgText.substring(pos, eol);
    const int eq = line.indexOf('=');
    if (eq > 0) {
      if (!first) o += ',';
      first = false;
      o += '"';
      o += line.substring(0, eq);
      o += "\":\"";
      o += line.substring(eq + 1);
      o += '"';
    }
    pos = eol + 1;
  }
  o += '}';
  return o;
}

static void feifanNoteLast(const String &id, const char *effect) {
  const size_t n = id.length() <= FEIFAN_TOKEN_ID ? id.length() : FEIFAN_TOKEN_ID;
  memcpy(feifanLastId, id.c_str(), n);
  feifanLastId[n] = '\0';
  snprintf(feifanLastEffect, sizeof(feifanLastEffect), "%s", effect);
}

// ── 真实生效的设置：全部在 feifan_native.h，本文件不再复制 ────────────

// 模块配置块的实时取值：只有读到过整块（cfgValid）才输出，
// 否则页面拿到空对象，不会用 0 覆盖自己存的值。
static String feifanModuleConfigJson() {
  String j = "{";
  if (tslCfgValid) {
    bool first = true;
    uint16_t len;
    portENTER_CRITICAL(&tslCfgMux);
    len = tslCfgLen;
    portEXIT_CRITICAL(&tslCfgMux);
    for (size_t i = 0; i < sizeof(TSL_MAP) / sizeof(TSL_MAP[0]); i++) {
      const TslBit &b = TSL_MAP[i];
      if (b.byte >= len) continue;
      uint8_t v = 0;
      if (!tslFieldGet(b.id, &v)) continue;
      if (!first) j += ',';
      first = false;
      j += '"';
      j += b.id;
      j += "\":\"";
      j += (unsigned long)v;
      j += '"';
    }
  }
  j += "}";
  return j;
}

static void httpFeifanStatus() {
  feifanCfgEnsureLoaded();
  String j;
  j.reserve(feifanCfgText.length() + 720);
  j = "{\"ok\":true,\"fw\":\"" FW_VERSION "\"";
  j += ",\"uptimeSec\":";
  j += (unsigned long)(millis() / 1000UL);
  j += ",\"freeHeap\":";
  j += (unsigned long)ESP.getFreeHeap();
  j += ",\"profile\":";
  j += (unsigned long)activeVehicleProfile;
  j += ",\"profileName\":\"";
  j += vehicleProfileName(activeVehicleProfile);
  j += "\",\"topologyName\":\"";
  j += vehicleProfileTopologyName(activeVehicleTopology);
  j += "\",\"setupMode\":";
  j += vehicleProfileSetupMode ? "true" : "false";
  j += ",\"config\":";
  j += feifanCfgJsonObject();
  // 三路 CAN 的真实在线状态：页面据此显示 3CAN / 部分 / 断开，不猜。
  j += ",\"canOnline\":[";
  for (uint8_t b = 0; b < P4_CAN_COUNT; b++) {
    if (b) j += ',';
    j += p4CanOnline[b] ? "1" : "0";
  }
  j += "],\"canRx\":[";
  for (uint8_t b = 0; b < P4_CAN_COUNT; b++) {
    if (b) j += ',';
    j += (unsigned long)p4CanRxCount[b];
  }
  j += "],\"canTxOk\":[";
  for (uint8_t b = 0; b < P4_CAN_COUNT; b++) {
    if (b) j += ',';
    j += (unsigned long)p4CanTxAccepted[b];
  }
  j += "],\"canTxFail\":[";
  for (uint8_t b = 0; b < P4_CAN_COUNT; b++) {
    if (b) j += ',';
    j += (unsigned long)p4CanTxRejected[b];
  }
  j += "]";
  j += ",\"applied\":";
  j += feifanNativeAppliedJson();
  // P4 原生设置的权威真值：页面重进时用这个覆盖本地缓存，
  // 而不是拿上次写入的 NVS 副本冒充车上的状态。
  j += ",\"nativeConfig\":";
  j += feifanNativeConfigJson();
  // SUPER_3CAN 移植项的运行时状态（开关 + 挡位/车速回读 + 发送计数）。
  j += ",\"s3Config\":";
  j += feifanS3ConfigJson();
  // 维护模式（Service Mode）：官方 v3.35.0 语义，0x339 单帧 + 0x284 回读。
  j += ",\"serviceConfig\":";
  j += feifanServiceConfigJson();
  // 灯光功能（作者 v3.35.6 整页）：AP 手动远光 / 远光频闪 / 后雾灯频闪 /
  // 动态刹车灯。独立页读 /api/lighting/stats，这里同时带一份，方便单请求诊断。
  j += ",\"lighting\":";
  j += ffLightingConfigJson();
  // TLSSC 免重启：缓存 0x331 + 每次上电补发，省掉「每轮上电重启车机」这一步。
  j += ",\"eapNr\":";
  j += feifanEapNrConfigJson();
  // 无模块标志：未启用链路且从未读到过配置块 = 车上没装 TSL 模块。
  // 页面据此把 44 项标「需模块」并锁定，不再显示「正在连接模块」。
  j += ",\"moduleAbsent\":";
  j += (!tslEnabled && !tslCfgValid) ? "true" : "false";
  // TSL 模块链路：44 个配置块位域与 0xA7/0xBB 动作都走这条 BLE 连接。
  j += ",\"module\":{\"enabled\":";
  j += tslEnabled ? "true" : "false";
  j += ",\"state\":\"";
  j += tslLinkStateName();
  j += "\",\"ready\":";
  j += tslReady() ? "true" : "false";
  j += ",\"cfgValid\":";
  j += tslCfgValid ? "true" : "false";
  j += ",\"cfgLen\":";
  j += (unsigned long)tslCfgLen;
  j += ",\"name\":\"";
  j += tslPeerName;
  j += "\",\"addr\":\"";
  j += tslPeerAddr;
  j += "\",\"error\":\"";
  j += tslLinkError;
  j += "\",\"writesOk\":";
  j += (unsigned long)tslWritesOk;
  j += ",\"writesFail\":";
  j += (unsigned long)tslWritesFail;
  j += ",\"actionsSent\":";
  j += (unsigned long)tslActionsSent;
  j += ",\"framesIn\":";
  j += (unsigned long)tslFramesIn;
  j += "}";
  j += ",\"moduleConfig\":";
  j += feifanModuleConfigJson();
  j += ",\"writesOk\":";
  j += (unsigned long)feifanWritesOk;
  j += ",\"writesFail\":";
  j += (unsigned long)feifanWritesFail;
  j += ",\"actionsBlocked\":";
  j += (unsigned long)feifanActionsBlocked;
  j += ",\"pending\":{\"action\":";
  j += tslReady() ? "false" : "true";
  j += ",\"gauge\":true,\"moduleBlob\":";
  j += tslCfgValid ? "false" : "true";
  j += "}";
  j += '}';
  server.send(200, "application/json", j);
}

static void httpFeifanConfigGet() {
  feifanCfgEnsureLoaded();
  String j;
  j.reserve(feifanCfgText.length() + 120);
  j = "{\"ok\":true,\"config\":";
  j += feifanCfgJsonObject();
  j += ",\"moduleConfig\":";
  j += feifanModuleConfigJson();
  j += ",\"applied\":";
  j += feifanNativeAppliedJson();
  j += ",\"nativeConfig\":";
  j += feifanNativeConfigJson();
  j += "}";
  server.send(200, "application/json", j);
}

static void httpFeifanConfigPost() {
  feifanCfgEnsureLoaded();
  const String body = server.arg("plain");
  String id;
  String value;
  const bool hasId = server.hasArg("id") || feifanJsonField(body, "id", id);
  const bool hasValue = server.hasArg("value") || feifanJsonField(body, "value", value);
  if (!hasId || !hasValue) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing-id-or-value\"}");
    return;
  }
  if (server.hasArg("id")) id = server.arg("id");
  if (server.hasArg("value")) value = server.arg("value");
  if (!feifanTokenOk(id, FEIFAN_TOKEN_ID) || !feifanTokenOk(value, FEIFAN_TOKEN_VAL)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"bad-token\"}");
    return;
  }
  if (vehicleProfileSetupMode) {
    server.send(409, "application/json",
                "{\"ok\":false,\"pending\":true,\"error\":\"profile-setup-mode\"}");
    return;
  }

  // 【1】P4 原生设置：本固件自己发 CAN 帧，不需要任何外挂硬件。
  // 优先级高于模块分支 —— 少数 id（如 a3a）同时存在于两张表里，
  // 有原生实现就用原生，别让用户装模块才能用。
  const uint8_t native = feifanNativeFor(id);
  if (native != FF_NATIVE_NONE) {
    const char *note = "native-apply-failed";
    const char *gateErr = nullptr;
    const bool applied = feifanNativeApply(native, value, note, gateErr);
    if (gateErr) {
      String g = "{\"ok\":false,\"pending\":true,\"error\":\"";
      g += gateErr;
      g += "\"}";
      server.send(409, "application/json", g);
      return;
    }
    // 会话级项不写 NVS：写下去重启后会变成"上次的值"，但车其实已恢复默认。
    // 其余原生项镜像一份，权威值由 status.nativeConfig 覆盖。
    const bool sessionOnly = (native == FF_NATIVE_PEDAL ||
                              native == FF_NATIVE_SUMMON_ON ||
                              native == FF_NATIVE_SUMMON_VAL);
    if (!sessionOnly) feifanCfgWrite(id, value);
    if (!applied) {
      feifanWritesFail++;
      feifanNoteLast(id, "failed");
      String e = "{\"ok\":false,\"pending\":true,\"error\":\"";
      e += note;
      e += "\"}";
      server.send(502, "application/json", e);
      return;
    }
    feifanWritesOk++;
    feifanNoteLast(id, "applied");
    String j;
    j.reserve(200);
    j = "{\"ok\":true,\"id\":\"";
    j += id;
    j += "\",\"value\":\"";
    j += value;
    j += "\",\"effect\":\"applied\",\"pending\":false,\"note\":\"";
    j += note;
    j += "\",\"via\":\"p4-native\"";
    if (sessionOnly) j += ",\"sessionOnly\":true";
    j += '}';
    server.send(200, "application/json", j);
    return;
  }

  // 【1c】TLSSC 免重启：更多功能里唯一一个必须跨上电保持的开关。
  // 它自己不发任何「新功能」帧，只是在每次上电后把缓存的 0x331 补丁
  // 提前铺到总线上，让车机启动时读到的就是 SELF_DRIVING，不用再重启车机。
  // 真正动手的前提是「被封车辆」+「TLSSC 恢复」都开着；这里只负责存开关，
  // 缺前提时如实回报 prereq=false，页面据此提示，不假装已经在工作。
  if (id == "eapNr") {
    const bool want = (value == "1" || value == "true" || value == "on");
    const char *note = "applied";
    if (!feifanEapNrSet(want, note)) {
      feifanWritesFail++;
      feifanNoteLast(id, "failed");
      String e = "{\"ok\":false,\"pending\":true,\"error\":\"";
      e += note;
      e += "\"}";
      server.send(502, "application/json", e);
      return;
    }
    feifanCfgWrite(id, want ? "1" : "0");  // 只做页面副本，权威值是 NVS 布尔
    feifanWritesOk++;
    feifanNoteLast(id, "applied");
    String je;
    je.reserve(260);
    je = "{\"ok\":true,\"id\":\"eapNr\",\"value\":\"";
    je += want ? "1" : "0";
    je += "\",\"effect\":\"applied\",\"pending\":false,\"persistent\":true"
          ",\"note\":\"stored-nvs\",\"via\":\"p4-native\",\"prereq\":";
    je += (bannedCar && tlsscRestoreEnabled) ? "true" : "false";
    je += ",\"banned\":";
    je += bannedCar ? "true" : "false";
    je += ",\"restore\":";
    je += tlsscRestoreEnabled ? "true" : "false";
    je += '}';
    server.send(200, "application/json", je);
    return;
  }

  // 【1b】SUPER_3CAN 移植项：读 stock 帧覆写，开关常驻 RAM。
  // 这些开关不写 NVS —— 车辆熄火重启后必须回到关闭状态，否则下次上电
  // 会在用户不知情的情况下继续发爆闪 / 换挡帧。
  const uint8_t s3 = feifanS3KindFor(id.c_str());
  if (s3 != FF_S3_NONE) {
    const bool want = (value == "1" || value == "true" || value == "on");
    const char *note = "applied";
    const bool ok = feifanS3Set(s3, want, note);
    if (!ok) {
      feifanWritesFail++;
      feifanNoteLast(id, "failed");
      String e = "{\"ok\":false,\"pending\":true,\"error\":\"";
      e += note;
      e += "\"}";
      server.send(502, "application/json", e);
      return;
    }
    feifanWritesOk++;
    feifanNoteLast(id, "applied");
    String js;
    js.reserve(200);
    js = "{\"ok\":true,\"id\":\"";
    js += id;
    js += "\",\"value\":\"";
    js += value;
    js += "\",\"effect\":\"applied\",\"pending\":false,\"note\":\"";
    js += note;
    js += "\",\"via\":\"p4-super3can\",\"sessionOnly\":true}";
    server.send(200, "application/json", js);
    return;
  }

  // 【2】TSL 模块配置块：需要车上装了 TSL 模块。
  // 没启用链路就直接如实回 module-absent，不再自动拉起 BLE 扫描
  // —— 无模块的设备一直扫描只会白耗资源，还让页面误以为"正在连接"。
  if (tslMapIndex(id.c_str()) >= 0) {
    const bool moduleOn = tslEnabled && tslReady();
    uint8_t want = 0;
    if (value == "true") want = 1;
    else if (value == "false") want = 0;
    else want = (uint8_t)value.toInt();
    const bool wrote = moduleOn ? tslFieldSet(id.c_str(), want) : false;
    if (!feifanCfgWrite(id, value)) {
      feifanWritesFail++;
      feifanNoteLast(id, "failed");
      server.send(502, "application/json",
                  "{\"ok\":false,\"error\":\"store-failed\"}");
      return;
    }
    feifanWritesOk++;
    feifanNoteLast(id, wrote ? "applied" : "stored");
    String jm;
    jm.reserve(210);
    jm = "{\"ok\":true,\"id\":\"";
    jm += id;
    jm += "\",\"value\":\"";
    jm += value;
    jm += "\",\"effect\":\"";
    jm += wrote ? "applied" : "stored";
    jm += "\",\"pending\":";
    jm += wrote ? "false" : "true";
    jm += ",\"note\":\"";
    jm += wrote ? "module-block-written"
                : (!tslEnabled ? "module-absent" : tslLinkStateName());
    jm += "\",\"moduleAbsent\":";
    jm += (!tslEnabled && !tslCfgValid) ? "true" : "false";
    jm += '}';
    server.send(200, "application/json", jm);
    return;
  }

  // 【3】其余项：只持久化，明确 pending，页面标「需新增」。
  if (!feifanCfgWrite(id, value)) {
    feifanWritesFail++;
    feifanNoteLast(id, "failed");
    server.send(502, "application/json",
                "{\"ok\":false,\"error\":\"store-failed\"}");
    return;
  }
  feifanWritesOk++;
  feifanNoteLast(id, "stored");
  String j;
  j.reserve(180);
  j = "{\"ok\":true,\"id\":\"";
  j += id;
  j += "\",\"value\":\"";
  j += value;
  j += "\",\"effect\":\"stored\",\"pending\":true,\"note\":\"stored-only\"}";
  server.send(200, "application/json", j);
}

// 首页快捷条与「模块开关」发的不是配置位域，是 0xBB 单字节键值。
static bool feifanKeyId(const String &id) {
  return id == "frontStrobe" || id == "rearStrobe" || id == "pulseHorn" ||
         id == "pilotMode" || id == "preheat" || id == "h2a" || id == "h2c";
}

// 动作码走模块：0xBB（快捷键值）/ 0xA7（车身动作）。
// 模块不在线就回 501，并说明是哪一步卡住，不假装发出去。
static void httpFeifanAction() {
  const String body = server.arg("plain");
  String id;
  String code;
  if (!feifanJsonField(body, "id", id)) id = server.hasArg("id") ? server.arg("id") : "";
  if (!feifanJsonField(body, "code", code)) feifanJsonField(body, "label", code);
  if (code.length() == 0 && server.hasArg("code")) code = server.arg("code");

  // 【0】维护模式（Service Mode）：对齐官方 v3.35.0 的 service_settings.h
  // 0x339 data[5] bit7，单帧、BODY 路、过 TX 屏障；每次点击只发一次，不重试不重放。
  if (id == "q1c" || id == "svc") {
    const esp_err_t err = feifanServiceEnter();
    const bool sent = (err == ESP_OK);
    feifanNoteLast(id, sent ? "applied" : "blocked");
    if (!sent) feifanActionsBlocked++;
    String a;
    a.reserve(220);
    a = "{\"ok\":";
    a += sent ? "true" : "false";
    a += ",\"pending\":";
    a += sent ? "false" : "true";
    a += ",\"reason\":\"";
    a += sent ? "service-frame-sent" : (err == ESP_ERR_INVALID_STATE ? "tx-gate-closed" : "can-tx-failed");
    a += "\",\"id\":\"";
    a += id;
    a += "\",\"code\":\"\",\"sentOnce\":true,\"blockedTotal\":";
    a += (unsigned long)feifanActionsBlocked;
    a += '}';
    server.send(sent ? 200 : 502, "application/json", a);
    return;
  }

  long n = -1;
  bool numeric = code.length() > 0;
  for (size_t i = 0; i < code.length(); i++) {
    if (!isdigit((unsigned char)code[i])) { numeric = false; break; }
  }
  if (numeric) n = code.toInt();

  // 无模块设备不再因为点一次快捷按钮就拉起 BLE 扫描。
  // 链路没启用 = 明确回 module-absent；启用了但没连上 = 回链路状态。
  const bool ready = tslEnabled && tslReady();
  bool sent = false;
  const char *note = !tslEnabled ? "module-absent"
                                 : (ready ? "module-not-ready" : tslLinkStateName());
  if (ready && numeric && n >= 0 && n <= 255) {
    sent = feifanKeyId(id) ? tslKeyAction((uint8_t)n) : tslBodyAction((uint8_t)n);
    note = sent ? (feifanKeyId(id) ? "key-sent" : "action-sent") : "module-write-failed";
  } else if (!numeric) {
    note = "action-code-missing";
  }

  feifanNoteLast(id, sent ? "applied" : "blocked");
  if (!sent) feifanActionsBlocked++;

  String j;
  j.reserve(220);
  j = "{\"ok\":";
  j += sent ? "true" : "false";
  j += ",\"pending\":";
  j += sent ? "false" : "true";
  j += ",\"reason\":\"";
  j += note;
  j += "\",\"module\":\"";
  j += tslLinkStateName();
  j += "\",\"id\":\"";
  j += feifanTokenOk(id, FEIFAN_TOKEN_ID) ? id : "";
  j += "\",\"code\":\"";
  j += feifanTokenOk(code, FEIFAN_TOKEN_VAL) ? code : "";
  j += "\",\"blockedTotal\":";
  j += (unsigned long)feifanActionsBlocked;
  j += '}';
  server.send(sent ? 200 : 501, "application/json", j);
}

static void httpFeifanGauge() {
  server.send(503, "application/json",
              "{\"ok\":false,\"pending\":true,\"reason\":\"gauge-decode-pending\","
              "\"note\":\"0xB0/0x5D9 payload decode is not implemented in 3.29.3\"}");
}

// 模块 256B 配置块 / gauge 快照目前没有 BLE 抓包来源，只回空 + pending；
// backup 是真实可用的：写 NVS，重启后还在。
static void httpFeifanBlobGet() {
  feifanCfgEnsureLoaded();
  String name = server.hasArg("name") ? server.arg("name") : String("quick");
  if (!feifanTokenOk(name, 8)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad-name\"}");
    return;
  }
  if (name == "backup") {
    String j;
    j.reserve(feifanCfgText.length() + 140);
    j = "{\"ok\":true,\"v\":1,\"fw\":\"" FW_VERSION "\",\"name\":\"backup\",\"config\":";
    j += feifanCfgJsonObject();
    j += '}';
    server.send(200, "application/json", j);
    return;
  }

  String hex;
  Preferences p;
  if (p.begin(FEIFAN_NVS_NS, true)) {
    hex = p.getString(("blob_" + name).c_str(), "");
    p.end();
  }
  String j;
  j.reserve(hex.length() + 160);
  j = "{\"ok\":true,\"name\":\"";
  j += name;
  j += "\",\"len\":";
  j += (unsigned long)(hex.length() / 2);
  j += ",\"hex\":\"";
  j += hex;
  j += "\",\"pending\":";
  j += hex.length() ? "false" : "true";
  if (!hex.length()) j += ",\"reason\":\"no-module-reply\"";
  j += '}';
  server.send(200, "application/json", j);
}

static void httpFeifanBlobPost() {
  const String name = server.hasArg("name") ? server.arg("name") : String("");
  if (!feifanTokenOk(name, 8) || name == "backup") {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad-name\"}");
    return;
  }
  String hex = server.arg("plain");
  hex.trim();
  if (hex.length() == 0 || hex.length() > 512 || (hex.length() % 2) != 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad-hex\"}");
    return;
  }
  for (size_t i = 0; i < hex.length(); i++) {
    const char c = hex[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad-hex\"}");
      return;
    }
  }
  Preferences p;
  if (!p.begin(FEIFAN_NVS_NS, false)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"nvs-open\"}");
    return;
  }
  const size_t written = p.putString(("blob_" + name).c_str(), hex);
  p.end();
  if (written == 0) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"nvs-write\"}");
    return;
  }
  server.send(200, "application/json",
              String("{\"ok\":true,\"name\":\"") + name + "\",\"len\":" +
                  String((unsigned)(hex.length() / 2)) + "}");
}

// 手动控制模块链路：页面上的「连接模块」开关走这里。
static void httpFeifanModule() {
  // GET 只查询状态：不能因为一次探活就把 BLE 链路拉起来。
  // 无模块是常态，扫描只会白耗资源，还会让页面误以为「正在连接模块」。
  // 只有 POST 且显式带 on 参数才允许切换链路开关。
  if (server.method() == HTTP_POST) {
    const String body = server.arg("plain");
    String on;
    bool have = false;
    bool want = false;
    if (feifanJsonField(body, "on", on)) {
      want = (on == "1" || on == "true");
      have = true;
    }
    if (server.hasArg("on")) {
      want = (server.arg("on") == "1" || server.arg("on") == "true");
      have = true;
    }
    if (have) {
      if (want) tslBegin(); else tslEnd();
    }
  }

  String j;
  j.reserve(260);
  j = "{\"ok\":true,\"enabled\":";
  j += tslEnabled ? "true" : "false";
  j += ",\"state\":\"";
  j += tslLinkStateName();
  j += "\",\"ready\":";
  j += tslReady() ? "true" : "false";
  j += ",\"cfgValid\":";
  j += tslCfgValid ? "true" : "false";
  j += ",\"name\":\"";
  j += tslPeerName;
  j += "\",\"error\":\"";
  j += tslLinkError;
  j += "\"}";
  server.send(200, "application/json", j);
}

static void feifanRegisterRoutes() {
  server.on("/api/feifan/status", HTTP_GET, httpFeifanStatus);
  server.on("/api/feifan/config", HTTP_GET, httpFeifanConfigGet);
  server.on("/api/feifan/config", HTTP_POST, httpFeifanConfigPost);
  server.on("/api/feifan/action", HTTP_POST, httpFeifanAction);
  server.on("/api/feifan/gauge", HTTP_GET, httpFeifanGauge);
  server.on("/api/feifan/blob", HTTP_GET, httpFeifanBlobGet);
  server.on("/api/feifan/blob", HTTP_POST, httpFeifanBlobPost);
  server.on("/api/feifan/module", HTTP_GET, httpFeifanModule);
  server.on("/api/feifan/module", HTTP_POST, httpFeifanModule);
}

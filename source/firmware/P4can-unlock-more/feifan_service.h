// ─────────────────────────────────────────────────────────────
// 维护模式（Service Mode）—— 对齐上游 P4-CAN Universal v3.35.0/3.35.3
//
// 上游 service_settings.h 的结论：
//   · 0x339 VCSEC_authentication，DLC 8，data[5] bit7 = 进入维护模式的请求；
//     其余字节全 0，不带特斯拉加和校验（这是 VCSEC 认证帧，不是普通车控帧）。
//   · **每次点击只发一帧**，不重试、不重放、不落盘。
//   · 车端是否真的进维护模式，只看 BODY 总线 0x284 UI_vehicleModes 的 d0 bit3。
//
// 和之前 q1c 的差别：SUPER_3CAN 用的是 data[5] = 0xE0（bit5/6/7 一起置位）连发 3 次，
// 官方只置 bit7 且只发一帧。以官方为准，多余位置位没有依据。
//
// 安全约束沿用上游：TX 屏障在、非管理员挂起、非 USB 被动、三路总线都跑着、
// 屏障 epoch 未变、BODY 总线不是 B 路时还要过 Summon 准入。
// ─────────────────────────────────────────────────────────────
#pragma once

static volatile uint32_t svcTxOk = 0;
static volatile uint32_t svcTxFail = 0;
static volatile uint32_t svcLastMs = 0;
static volatile int8_t svcLastErr = 0;
static volatile bool svcConfirmed = false;    // 0x284 d0 bit3：车端回报已进入
static volatile uint32_t svcConfirmMs = 0;    // 最近一次看到该位的时间
static volatile uint32_t svcRx284 = 0;        // 累计收到的 0x284 帧数
static volatile uint32_t svcRx339 = 0;        // 累计收到的 0x339 帧数

// 一次点击 = 一次发送尝试。失败不重试，由页面提示「未确认」。
static esp_err_t feifanServiceEnter() {
  const uint32_t epoch = canTxEpochSnapshot();
  if (!canTxBarrierMutex ||
      xSemaphoreTake(canTxBarrierMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_INVALID_STATE;

  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (!canTxAdministrativeHold && !canUsbPassive() && p4CanAllRunning() &&
      epoch != 0u && epoch == canTxBarrierState.epoch &&
      (p4CanBusB() != P4_BODY || twaiNonSummonAdmissionOpen())) {
    CanFrame out;
    out.bus = P4_BODY;
    out.can_id = 0x339u;
    out.can_dlc = 8;
    memset(out.data, 0, sizeof(out.data));
    out.data[5] = 0x80u;   // byte5 bit7 = service-mode request
    err = p4CanSendFrame(&out);
  }
  xSemaphoreGive(canTxBarrierMutex);

  svcLastErr = (int8_t)err;
  svcLastMs = (uint32_t)millis();
  if (err == ESP_OK) svcTxOk++;
  else svcTxFail++;
  return err;
}

// BODY 路观察：0x284 的 d0 bit3 是车端回读的 UI_serviceMode。
static void feifanServiceObserve(uint32_t id, const uint8_t *d, uint8_t n) {
  if (!d || n == 0) return;
  if (id == 0x339u) { svcRx339++; return; }
  if (id == 0x284u) {
    svcRx284++;
    if (n < 1) return;
    if ((d[0] >> 3) & 0x01) {
      svcConfirmed = true;
      svcConfirmMs = (uint32_t)millis();
    } else if (svcConfirmed) {
      svcConfirmed = false;
    }
  }
}

static String feifanServiceConfigJson() {
  String j = "{";
  j += "\"txOk\":"; j += String((unsigned long)svcTxOk);
  j += ",\"txFail\":"; j += String((unsigned long)svcTxFail);
  j += ",\"lastMs\":"; j += String((unsigned long)svcLastMs);
  j += ",\"lastErr\":"; j += String((int)svcLastErr);
  j += ",\"confirmed\":"; j += svcConfirmed ? "true" : "false";
  j += ",\"confirmMs\":"; j += String((unsigned long)svcConfirmMs);
  j += ",\"rx284\":"; j += String((unsigned long)svcRx284);
  j += ",\"rx339\":"; j += String((unsigned long)svcRx339);
  j += "}";
  return j;
}

#pragma once

// TSL module BLE client.
//
// The 更多功能 page is a merge of two sources: this firmware's own CAN code and
// the TSL module's settings.  The module is a BLE peripheral (service FFF0 /
// characteristic FFF1); its 44 writable settings live in one config block that
// is read back with CMD 0xAB and written whole with CMD 0xA3.
//
// Frame layout (docs/PROTOCOL.md):
//
//   55 7F CMD LEN_H LEN_L [payload] SUM
//   SUM = (CMD + LEN_H + LEN_L + sum(payload)) & 0xFF
//
// Everything here is gated on a live link.  A field write that never reaches a
// connected module reports pending instead of pretending it applied, which is
// the whole point of the endpoint contract in feifan_api.h.

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>

#define TSL_SVC_UUID "0000fff0-0000-1000-8000-00805f9b34fb"
#define TSL_CHR_UUID "0000fff1-0000-1000-8000-00805f9b34fb"

// MTU 23 leaves 20 usable bytes per write.
static constexpr uint16_t TSL_CHUNK_BYTES = 20;
static constexpr uint16_t TSL_CFG_MAX = 256;
static constexpr uint16_t TSL_MIN_CFG_LEN = 35;
static constexpr uint32_t TSL_SCAN_MS = 6000;
static constexpr uint32_t TSL_RETRY_MS = 8000;
static constexpr uint32_t TSL_CFG_STALE_MS = 30000;

static constexpr uint8_t TSL_CMD_INFO = 0xA0;
static constexpr uint8_t TSL_CMD_INFO_EXT = 0xA1;
static constexpr uint8_t TSL_CMD_WRITE_CFG = 0xA3;
static constexpr uint8_t TSL_CMD_BODY_ACTION = 0xA7;
static constexpr uint8_t TSL_CMD_QUICK_BLOCK = 0xAB;
static constexpr uint8_t TSL_CMD_KEY = 0xBB;

enum TslLinkState : uint8_t {
  TSL_LINK_OFF = 0,      // feature not enabled
  TSL_LINK_BLOCKED,      // another BLE owner (S3XY mapper) holds the stack
  TSL_LINK_WAIT_NET,     // network stack not up yet
  TSL_LINK_SCANNING,
  TSL_LINK_CONNECTING,
  TSL_LINK_HANDSHAKE,
  TSL_LINK_READY,
  TSL_LINK_ERROR,
};

struct TslBit {
  const char *id;
  uint8_t byte;
  uint8_t shift;
  uint8_t mask;
  int8_t bias;     // moduleValue = pageValue + bias
  uint8_t maxOut;  // highest value the module accepts for this field
};

static volatile uint8_t tslLinkState = TSL_LINK_OFF;
static volatile bool tslEnabled = false;
static volatile bool tslForceScan = false;
static char tslLinkError[72] = "";
static char tslPeerName[40] = "";
static char tslPeerAddr[20] = "";

static uint8_t tslCfg[TSL_CFG_MAX];
static uint16_t tslCfgLen = 0;
static bool tslCfgValid = false;
static uint32_t tslCfgStampMs = 0;

static uint32_t tslWritesOk = 0;
static uint32_t tslWritesFail = 0;
static uint32_t tslActionsSent = 0;
static uint32_t tslFramesIn = 0;

static BLEClient *tslClient = nullptr;
static BLERemoteCharacteristic *tslChar = nullptr;
static uint32_t tslNextAttemptMs = 0;
static bool tslScanRequested = false;

static portMUX_TYPE tslCfgMux = portMUX_INITIALIZER_UNLOCKED;

// Incoming frames are reassembled here: notifications arrive in 20-byte pieces.
static uint8_t tslRxBuf[600];
static uint8_t tslInfoRounds = 0;
static uint16_t tslRxLen = 0;

#include "tsl_map.h"

static void tslSetError(const char *msg) {
  size_t n = strlen(msg ? msg : "");
  if (n >= sizeof(tslLinkError)) n = sizeof(tslLinkError) - 1;
  memcpy(tslLinkError, msg ? msg : "", n);
  tslLinkError[n] = '\0';
  tslLinkState = TSL_LINK_ERROR;
}

static const char *tslLinkStateName() {
  switch (tslLinkState) {
    case TSL_LINK_OFF: return "off";
    case TSL_LINK_BLOCKED: return "ble-busy";
    case TSL_LINK_WAIT_NET: return "wait-net";
    case TSL_LINK_SCANNING: return "scanning";
    case TSL_LINK_CONNECTING: return "connecting";
    case TSL_LINK_HANDSHAKE: return "handshake";
    case TSL_LINK_READY: return "ready";
    case TSL_LINK_ERROR: return "error";
  }
  return "unknown";
}

static bool tslReady() {
  return tslLinkState == TSL_LINK_READY && tslChar != nullptr;
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

static bool tslWriteAll(const uint8_t *data, uint16_t len) {
  if (!tslChar) return false;
  uint8_t chunk[TSL_CHUNK_BYTES];
  for (uint16_t off = 0; off < len; off += TSL_CHUNK_BYTES) {
    uint16_t n = len - off;
    if (n > TSL_CHUNK_BYTES) n = TSL_CHUNK_BYTES;
    memcpy(chunk, data + off, n);
    // Write-with-response so the queue stays ordered; the module drops bytes
    // when two writes land in the same connection event.
    if (!tslChar->writeValue(chunk, n, true)) return false;
    delay(4);
  }
  return true;
}

static bool tslSendFrame(uint8_t cmd, const uint8_t *payload, uint16_t len) {
  if (!tslChar || len > 250) return false;
  uint8_t frame[260];
  uint16_t i = 0;
  frame[i++] = 0x55;
  frame[i++] = 0x7F;
  frame[i++] = cmd;
  frame[i++] = (uint8_t)((len >> 8) & 0xFF);
  frame[i++] = (uint8_t)(len & 0xFF);
  uint32_t sum = (uint32_t)cmd + ((len >> 8) & 0xFF) + (len & 0xFF);
  for (uint16_t k = 0; k < len; k++) {
    frame[i++] = payload[k];
    sum += payload[k];
  }
  frame[i++] = (uint8_t)(sum & 0xFF);
  return tslWriteAll(frame, i);
}

// ---------------------------------------------------------------------------
// Config block
// ---------------------------------------------------------------------------

static bool tslConfigRequest() {
  if (!tslChar) return false;
  return tslSendFrame(TSL_CMD_QUICK_BLOCK, nullptr, 0);
}

static bool tslConfigWriteBack() {
  if (!tslChar) return false;
  uint16_t len;
  portENTER_CRITICAL(&tslCfgMux);
  len = tslCfgLen;
  portEXIT_CRITICAL(&tslCfgMux);
  // Never invent a length: echo back exactly what the module handed us.
  if (len < TSL_MIN_CFG_LEN || len > TSL_CFG_MAX) return false;
  bool ok = tslSendFrame(TSL_CMD_WRITE_CFG, tslCfg, len);
  if (ok) tslWritesOk++; else tslWritesFail++;
  if (ok) tslCfgStampMs = millis();
  return ok;
}

static int tslMapIndex(const char *id) {
  if (!id) return -1;
  for (size_t i = 0; i < sizeof(TSL_MAP) / sizeof(TSL_MAP[0]); i++) {
    if (strcmp(TSL_MAP[i].id, id) == 0) return (int)i;
  }
  return -1;
}

static bool tslFieldGet(const char *id, uint8_t *out) {
  int idx = tslMapIndex(id);
  if (idx < 0 || !out) return false;
  const TslBit &b = TSL_MAP[idx];
  bool ok = false;
  portENTER_CRITICAL(&tslCfgMux);
  if (tslCfgValid && b.byte < tslCfgLen) {
    uint8_t raw = (uint8_t)((tslCfg[b.byte] >> b.shift) & b.mask);
    int v = (int)raw - (int)b.bias;
    if (v < 0) v = 0;
    *out = (uint8_t)v;
    ok = true;
  }
  portEXIT_CRITICAL(&tslCfgMux);
  return ok;
}

// Read-modify-write one bitfield, then push the whole block back (CMD 0xA3).
static bool tslFieldSet(const char *id, uint8_t value) {
  int idx = tslMapIndex(id);
  if (idx < 0) return false;
  const TslBit &b = TSL_MAP[idx];
  int v = (int)value + (int)b.bias;
  // Refuse out-of-range values instead of letting them spill into the
  // neighbouring bitfields.
  if (v < 0 || v > (int)b.maxOut || v > (int)b.mask) return false;
  portENTER_CRITICAL(&tslCfgMux);
  bool usable = tslCfgValid && b.byte < tslCfgLen;
  if (usable) {
    uint8_t cur = tslCfg[b.byte];
    tslCfg[b.byte] = (uint8_t)((cur & ~(b.mask << b.shift)) |
                               (((uint8_t)v & b.mask) << b.shift));
  }
  portEXIT_CRITICAL(&tslCfgMux);
  if (!usable) return false;
  return tslConfigWriteBack();
}

// ---------------------------------------------------------------------------
// Incoming frames
// ---------------------------------------------------------------------------

static void tslHandleFrame(uint8_t cmd, const uint8_t *payload, uint16_t len) {
  tslFramesIn++;
  if (cmd == TSL_CMD_QUICK_BLOCK || cmd == TSL_CMD_WRITE_CFG) {
    if (len >= TSL_MIN_CFG_LEN) {
      uint16_t n = len > TSL_CFG_MAX ? TSL_CFG_MAX : len;
      portENTER_CRITICAL(&tslCfgMux);
      memcpy(tslCfg, payload, n);
      tslCfgLen = n;
      tslCfgValid = true;
      portEXIT_CRITICAL(&tslCfgMux);
      tslCfgStampMs = millis();
      if (tslLinkState == TSL_LINK_HANDSHAKE) tslLinkState = TSL_LINK_READY;
    }
  } else if (cmd == TSL_CMD_INFO && tslLinkState == TSL_LINK_HANDSHAKE) {
    // The module answers 0xA0 twice before it accepts block writes.
    tslInfoRounds++;
    if (tslInfoRounds >= 2) {
      tslConfigRequest();
    } else {
      tslSendFrame(TSL_CMD_INFO, nullptr, 0);
    }
  }
}

static void tslDispatchRx() {
  while (tslRxLen >= 6) {
    // Resync on the 55 7F header.
    if (tslRxBuf[0] != 0x55 || tslRxBuf[1] != 0x7F) {
      uint16_t k = 1;
      while (k + 1 < tslRxLen && !(tslRxBuf[k] == 0x55 && tslRxBuf[k + 1] == 0x7F)) k++;
      if (k + 1 >= tslRxLen) { tslRxLen = 0; return; }
      memmove(tslRxBuf, tslRxBuf + k, tslRxLen - k);
      tslRxLen -= k;
      continue;
    }
    uint8_t cmd = tslRxBuf[2];
    uint16_t len = ((uint16_t)tslRxBuf[3] << 8) | tslRxBuf[4];
    uint16_t total = (uint16_t)(5 + len + 1);
    if (len > 250 || tslRxLen < total) {
      if (tslRxLen >= sizeof(tslRxBuf)) tslRxLen = 0;  // garbage, drop it
      return;
    }
    uint32_t sum = (uint32_t)cmd + tslRxBuf[3] + tslRxBuf[4];
    for (uint16_t i = 0; i < len; i++) sum += tslRxBuf[5 + i];
    if ((uint8_t)(sum & 0xFF) == tslRxBuf[5 + len]) {
      tslHandleFrame(cmd, tslRxBuf + 5, len);
    }
    memmove(tslRxBuf, tslRxBuf + total, tslRxLen - total);
    tslRxLen -= total;
  }
}

static void tslNotifyCb(BLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
  (void)chr;
  (void)isNotify;
  if (!data || !len) return;
  if (tslRxLen + len > sizeof(tslRxBuf)) tslRxLen = 0;
  memcpy(tslRxBuf + tslRxLen, data, len);
  tslRxLen += (uint16_t)len;
  tslDispatchRx();
}

// ---------------------------------------------------------------------------
// Link management
// ---------------------------------------------------------------------------

class TslClientCallbacks : public BLEClientCallbacks {
 public:
  void onConnect(BLEClient *client) override {
    (void)client;
    tslLinkState = TSL_LINK_HANDSHAKE;
  }
  void onDisconnect(BLEClient *client) override {
    (void)client;
    tslChar = nullptr;
    tslCfgValid = false;
    portENTER_CRITICAL(&tslCfgMux);
    tslCfgLen = 0;
    portEXIT_CRITICAL(&tslCfgMux);
    if (tslLinkState != TSL_LINK_ERROR) tslLinkState = TSL_LINK_SCANNING;
    tslNextAttemptMs = millis() + TSL_RETRY_MS;
  }
};

static TslClientCallbacks tslClientCallbacks;

class TslAdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice device) override {
    if (tslLinkState == TSL_LINK_READY || tslLinkState == TSL_LINK_CONNECTING) return;
    bool matches = device.haveServiceUUID() && device.isAdvertisingService(BLEUUID(TSL_SVC_UUID));
    if (!matches) {
      String nm = device.getName().c_str();
      nm.toUpperCase();
      matches = nm.indexOf("TSL") >= 0;
    }
    if (!matches) return;
    String nm = device.getName().c_str();
    strncpy(tslPeerName, nm.length() ? nm.c_str() : "(unnamed)", sizeof(tslPeerName) - 1);
    tslPeerName[sizeof(tslPeerName) - 1] = '\0';
    strncpy(tslPeerAddr, device.getAddress().toString().c_str(), sizeof(tslPeerAddr) - 1);
    tslPeerAddr[sizeof(tslPeerAddr) - 1] = '\0';
    BLEScan *scan = BLEDevice::getScan();
    if (scan) scan->stop();
    tslLinkState = TSL_LINK_CONNECTING;
    tslScanRequested = false;
  }
};

static TslAdvertisedCallbacks tslAdvertisedCallbacks;

static void tslDropClient() {
  tslChar = nullptr;
  // Keep the client object: BLEDevice holds one client per create and a failed
  // connect leaves it allocated, so reuse it instead of leaking a new one.
  if (tslClient && tslClient->isConnected()) tslClient->disconnect();
}

static bool tslConnectPeer() {
  if (!tslPeerAddr[0]) return false;
  if (!tslClient) {
    tslClient = BLEDevice::createClient();
    if (!tslClient) {
      tslSetError("createClient failed");
      return false;
    }
    tslClient->setClientCallbacks(&tslClientCallbacks);
  } else if (tslClient->isConnected()) {
    tslClient->disconnect();
  }
  BLEAddress addr(tslPeerAddr);
  if (!tslClient->connect(addr)) {
    tslSetError("connect failed");
    tslDropClient();
    return false;
  }
  BLERemoteService *svc = tslClient->getService(BLEUUID(TSL_SVC_UUID));
  if (!svc) {
    tslSetError("FFF0 service missing");
    tslDropClient();
    return false;
  }
  tslChar = svc->getCharacteristic(BLEUUID(TSL_CHR_UUID));
  if (!tslChar) {
    tslSetError("FFF1 characteristic missing");
    tslDropClient();
    return false;
  }
  if (!tslChar->canNotify()) {
    tslSetError("FFF1 cannot notify");
    tslDropClient();
    return false;
  }
  tslChar->registerForNotify(tslNotifyCb);
  tslCfgValid = false;
  tslRxLen = 0;
  tslInfoRounds = 0;
  tslLinkState = TSL_LINK_HANDSHAKE;
  tslSendFrame(TSL_CMD_INFO, nullptr, 0);
  return true;
}

static void tslStartScan() {
  BLEScan *scan = BLEDevice::getScan();
  if (!scan) {
    tslSetError("scan object unavailable");
    return;
  }
  scan->setAdvertisedDeviceCallbacks(&tslAdvertisedCallbacks);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->clearResults();
  tslLinkState = TSL_LINK_SCANNING;
  tslScanRequested = true;
  tslNextAttemptMs = millis() + TSL_SCAN_MS + TSL_RETRY_MS;
  scan->start(TSL_SCAN_MS, nullptr, false);
}

static bool tslBegin() {
  if (tslEnabled) return tslLinkState == TSL_LINK_READY;
  if (!p4NetworkAllowsBle()) {
    tslLinkState = TSL_LINK_WAIT_NET;
    tslNextAttemptMs = millis() + 2000;
    tslEnabled = true;
    return false;
  }
  // The C6 is a single BLE host: the S3XY mapper owns BLEDevice when it is on.
  if (s3xyBleInitialized) {
    tslSetError("S3XY mapper owns BLE; turn it off to use the module link");
    tslLinkState = TSL_LINK_BLOCKED;
    return false;
  }
  if (!BLEDevice::getInitialized()) {
    if (!BLEDevice::init("P4-CAN_TSL")) {
      tslSetError("BLE host init failed");
      return false;
    }
    BLEDevice::setMTU(23);
  }
  tslEnabled = true;
  tslLinkError[0] = '\0';
  tslStartScan();
  return true;
}

static void tslEnd() {
  tslEnabled = false;
  tslDropClient();
  tslCfgValid = false;
  tslLinkState = TSL_LINK_OFF;
  tslPeerAddr[0] = '\0';
}

static void tslLoop() {
  if (!tslEnabled) return;
  uint32_t now = millis();

  if (tslLinkState == TSL_LINK_WAIT_NET) {
    if (!p4NetworkAllowsBle()) return;
    tslEnabled = false;
    tslBegin();
    return;
  }

  if (tslLinkState == TSL_LINK_READY) {
    if (!tslClient || !tslClient->isConnected()) {
      tslChar = nullptr;
      tslLinkState = TSL_LINK_SCANNING;
      tslNextAttemptMs = now + TSL_RETRY_MS;
      return;
    }
    if (tslCfgValid && (now - tslCfgStampMs) > TSL_CFG_STALE_MS) {
      tslConfigRequest();  // keep the cache honest; module state can change
    }
    return;
  }

  if (tslLinkState == TSL_LINK_SCANNING && !tslScanRequested && now >= tslNextAttemptMs) {
    tslStartScan();
    return;
  }

  if (tslLinkState == TSL_LINK_CONNECTING) {
    tslConnectPeer();
    return;
  }

  if (tslLinkState == TSL_LINK_ERROR && now >= tslNextAttemptMs) {
    tslLinkError[0] = '\0';
    tslLinkState = TSL_LINK_SCANNING;
    tslScanRequested = false;
  }
}

// ---------------------------------------------------------------------------
// Public actions used by feifan_api.h
// ---------------------------------------------------------------------------

static bool tslBodyAction(uint8_t code) {
  if (!tslReady()) return false;
  if (!tslSendFrame(TSL_CMD_BODY_ACTION, &code, 1)) return false;
  tslActionsSent++;
  return true;
}

static bool tslKeyAction(uint8_t value) {
  if (!tslReady()) return false;
  if (!tslSendFrame(TSL_CMD_KEY, &value, 1)) return false;
  tslActionsSent++;
  return true;
}

// BLEScan::start() blocks for the whole scan window, so the link state machine
// must not run on the web task: a 6 s scan would freeze every page request.
static void tslLinkTask(void *arg) {
  (void)arg;
  for (;;) {
    tslLoop();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static void tslStartTask() {
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreate(tslLinkTask, "tslLink", 6144, nullptr, 1, nullptr);
}

#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// 抓包（Packet capture）—— 独立于 LAB 的「研究抓包」
// ═══════════════════════════════════════════════════════════════════════════
//
// 为什么不复用 can_research_capture.h：那套是给 A–F 场景标注、前后窗口、
// 自动触发用的，状态机很重，而且它只保留触发点附近的行。这里要的是最朴素
// 的东西：按一下开始、按一下停、把期间收到的帧整段导出来。
//
// 设计：
//   · 环缓冲，满了覆盖最旧的一行（连续抓不会因为不按停止就丢新帧）；
//   · 优先放 PSRAM（16k 行 ≈ 256 KB），没有 PSRAM 就退到内部 RAM 的 2k 行；
//   · 每一行 16 字节：ms / id / bus / dlc / data[8]，导出时按 CAN 习惯排；
//   · 只读、绝不发送任何帧；开关关闭时观察函数只做一次 bool 判断。
// ═══════════════════════════════════════════════════════════════════════════

#include <esp_heap_caps.h>

#define FF_CAP_LIMIT_PSRAM 16384u
#define FF_CAP_LIMIT_RAM 2048u
#define FF_CAP_RING_SLOTS ((FF_CAP_LIMIT_PSRAM))

struct FfCapEntry {
  uint32_t ms;
  uint16_t id;
  uint8_t bus;
  uint8_t dlc;
  uint8_t d[8];
};

static FfCapEntry *ffCapBuf = nullptr;
static uint32_t ffCapCap = 0;          // 实际分配的槽数
static volatile bool ffCapOn = false;
static volatile uint32_t ffCapHead = 0;   // 下一个写入位置
static volatile uint32_t ffCapTotal = 0;  // 本次抓包累计写入（含被覆盖的）
static volatile uint32_t ffCapSeq = 0;    // 全局序号（清空也不回退）
static volatile uint8_t ffCapBusMask = 0x07;  // bit0=A bit1=B bit2=C
static volatile uint16_t ffCapId = 0;         // 0 = 不过滤
static volatile bool ffCapPsram = false;
static volatile uint32_t ffCapStartMs = 0;
static portMUX_TYPE ffCapMux = portMUX_INITIALIZER_UNLOCKED;

static bool ffCapAlloc() {
  if (ffCapBuf) return true;
  const size_t bytes = sizeof(FfCapEntry) * FF_CAP_LIMIT_PSRAM;
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    ffCapBuf = (FfCapEntry *)p;
    ffCapCap = FF_CAP_LIMIT_PSRAM;
    ffCapPsram = true;
    return true;
  }
  p = heap_caps_malloc(sizeof(FfCapEntry) * FF_CAP_LIMIT_RAM, MALLOC_CAP_8BIT);
  if (!p) return false;
  ffCapBuf = (FfCapEntry *)p;
  ffCapCap = FF_CAP_LIMIT_RAM;
  ffCapPsram = false;
  return true;
}

// 热路径：每个收到的帧调一次。关闭时只有一次 bool 读。
//
// 这里必须进临界区：BODY/CHASSIS/PARTY 各有自己的接收任务，两路帧在同一微秒
// 到达是常事（尤其 500 kbps 满负载）。head 的自增是读-改-写，不加锁会让两帧
// 抢同一个槽 —— 结果是一条记录里混着两个 ID 的字节，导出的 CSV 反而更难看懂。
// 临界区只有十几次字节拷贝，满负载下也占不到 1% CPU。
static inline void ffCapObserve(uint8_t bus, uint16_t id, uint8_t dlc, const uint8_t *d) {
  if (!ffCapOn || !ffCapBuf || bus > 2 || !d || dlc > 8) return;
  if (!(ffCapBusMask & (1u << bus))) return;
  const uint16_t want = ffCapId;
  if (want && id != want) return;
  portENTER_CRITICAL(&ffCapMux);
  const uint32_t slot = ffCapHead;
  FfCapEntry &e = ffCapBuf[slot];
  e.ms = (uint32_t)millis();
  e.id = id;
  e.bus = bus;
  e.dlc = dlc;
  for (uint8_t i = 0; i < dlc; i++) e.d[i] = d[i];
  for (uint8_t i = dlc; i < 8; i++) e.d[i] = 0;
  ffCapHead = (slot + 1u) % ffCapCap;
  ffCapTotal = (uint32_t)(ffCapTotal + 1u);
  ffCapSeq = (uint32_t)(ffCapSeq + 1u);
  portEXIT_CRITICAL(&ffCapMux);
}

static bool ffCapStart(uint8_t busMask, uint16_t idFilter) {
  if (!ffCapAlloc()) return false;
  ffCapBusMask = (uint8_t)(busMask & 0x07u);
  if (!ffCapBusMask) ffCapBusMask = 0x07u;
  ffCapId = idFilter;
  portENTER_CRITICAL(&ffCapMux);
  ffCapTotal = 0;
  ffCapHead = 0;
  portEXIT_CRITICAL(&ffCapMux);
  ffCapStartMs = (uint32_t)millis();
  ffCapOn = true;
  return true;
}

static void ffCapStop() { ffCapOn = false; }

static void ffCapClear() {
  // 清空时可能还有帧在路上，head/total 一起改必须进临界区
  portENTER_CRITICAL(&ffCapMux);
  ffCapTotal = 0;
  ffCapHead = 0;
  portEXIT_CRITICAL(&ffCapMux);
}

// 抓包里最旧一行的全局序号（导出 / 分页用）
static uint32_t ffCapFirstSeq() {
  const uint32_t total = ffCapTotal;
  return (total > ffCapCap) ? (total - ffCapCap) : 0u;
}

# Fix: TLSSC-restore-induced MCU reboot on every power cycle (`0x331` cached replay)

> Proposed upstream patch for **P4can-universal-unlock v3.29.3**.
> Base: `06066060606060/P4can-universal-unlock` @ `v3.29.3`.
> This document is the PR description; it explains the problem, the root cause,
> the fix, how to verify it, and the known limits.

---

## TL;DR

With **Banned Car** + **TLSSC Restore** enabled, the touchscreen (MCU) had to be
restarted by hand after *every* power-up for TLSSC to come back. The stock
restore logic is purely reactive — it patches `0x331` only after the car emits
one — but the MCU reads and latches its DAS configuration once at its own boot.
If no patched `0x331` is on the bus at that instant, the MCU latches the
"banned" state. The fix caches the patched frame in NVS and replays it for a
bounded window right after CAN B comes up, so the frame is already on the bus
before/when the MCU reads. A follow-up fix stops a TX-barrier lock collision
from being misreported as a replay failure.

---

## Problem

### Symptom

- Enable **Settings → Banned Car → TLSSC Restore**.
- On every power cycle, TLSSC is unavailable until the touchscreen is manually
  rebooted. Reboot it once, TLSSC works; next power-up, the same again.
- Upstream Flipper documentation labels this exact behavior *"Triggers MCU
  reboot"*.

### Root cause

The existing handler, `doInjectTlsscRestore()` in `vehicle_logic.h`, does a
read-modify-retransmit of `0x331 DAS_autopilotConfig` (Standard 3/Y only):

```c
static void doInjectTlsscRestore(const twai_message_t &src) {
    if (!activeProfileTlsscRestoreSupported()) return;
    if (!bannedCar || !tlsscRestoreEnabled) return;
    if (src.extd || src.rtr || src.data_length_code < 1) return;
    const uint8_t patched0 = (src.data[0] & 0xC0U) | 0x1BU;  // SELF_DRIVING
    if (patched0 == src.data[0]) return;
    if (!twaiNonSummonAdmissionOpen()) return;
    twai_message_t out = src;
    out.flags = 0;
    out.data[0] = patched0;
    (void)canTxTwaiTransmit(&out, canTxEpochSnapshot());
}
```

It only runs **in response to a stock `0x331` frame**. The MCU, however, reads
and latches its DAS configuration **once, during its own boot**. Two timing
cases therefore fail:

1. The MCU boots and reads the config *before* the car emits the first `0x331`
   we could patch — it latches "banned" and will not re-evaluate until rebooted.
2. Even if a stock `0x331` arrives later, the reactive path has already missed
   the one moment that mattered.

This is a bus-timing problem, not a bad patch value: the patched payload
(`byte0` high two bits preserved, low six forced to `0x1B`) is correct. What is
missing is a patched frame that is **already on the bus at MCU boot**.

---

## Proposed fix

A self-contained module, `feifan_eap_norestart.h`, plus two small hooks into the
existing CAN runtime. It does not alter `doInjectTlsscRestore()` itself — the
reactive path stays intact and unchanged.

### Design

| Step | Behavior |
|---|---|
| **Learn** | On every stock `0x331` seen on CAN B, compute the patched payload (`(data[0] & 0xC0) \| 0x1B`) and hold it as a candidate cache frame. |
| **Cache** | Persist the patched payload to NVS (`feifan/eapHex`), at most one write per power cycle to protect flash. Survives power-off. |
| **Replay** | On each power-up, once CAN B is ready, open a **60 s** window and re-send the cached frame at **10 Hz** (every 100 ms) — without waiting for the car to speak first. |
| **Yield** | If the car itself emits a fresh `0x331` (seen within 2 s), stop replaying immediately; the existing reactive path already covers that case, so we do not fight the real frame for the bus. |
| **Stop** | End the window when it expires or after **600** frames. Never replay indefinitely. |

Key constants:

```c
#define FEIFAN_EAP_NR_WINDOW_MS      60000u  // replay window per power-up
#define FEIFAN_EAP_NR_PERIOD_MS        100u  // 10 Hz
#define FEIFAN_EAP_NR_MAX_FRAMES       600u  // hard frame cap
#define FEIFAN_EAP_NR_STOCK_FRESH_MS  2000u  // fresh stock frame -> yield
```

### Why a separate module instead of editing `doInjectTlsscRestore()`

- The stock handler is **reactive by contract**; making it proactively inject
  would change its semantics for every user, including those who never enable
  this option.
- The fix needs state that the handler has no concept of: a **cross-power-cycle
  NVS cache**, a boot-time **window/timer**, a **yield decision** against stock
  traffic, and its own **enable switch + telemetry**.
- Keeping it separate keeps the patch opt-in and reviewable: two hooks in
  `can_runtime.h` (one in the CAN B receive path, one in the periodic service
  section) and one status/config hook in `feifan_api.h`.

### Integration points

| File | Change |
|---|---|
| `firmware/P4can-unlock-more/feifan_eap_norestart.h` | **New.** Cache, replay state machine, NVS persistence, JSON telemetry. |
| `can_runtime.h` | Call `feifanEapNrObserve331(f)` in the CAN B `0x331` receive branch; call `feifanEapNrTick()` from the periodic service section. |
| `feifan_api.h` | Expose `feifanEapNrConfigJson()` in `status`; handle the enable/disable action via `feifanEapNrSet()`. |
| `P4can-unlock-more.ino`, `dashboard_source.html`, `tools/more/*`, `sim/` | UI switch, status row, docs/sim generation (project-specific; the core fix is the three files above). |

### Guards and safety

The replay path is gated on **all** of the following; any missing condition
means "store the switch, send nothing":

- feature switch `eapNr` enabled (persisted in NVS under `feifan/eapNr`);
- **both** `bannedCar` and `tlsscRestoreEnabled` active;
- CAN B currently mapped to CHASSIS;
- TWAI ready, no admin hold, not in USB-passive mode;
- epoch unchanged, and a valid cached frame exists.

With no cached frame (first use) it sends nothing and reports "waiting for
first frame" — no frame is ever synthesized from guesswork. All send failures
are counted only; they never block the CAN task.

---

## Follow-up fix: "45 frames replayed, 1 failed"

After the initial fix, the status line occasionally showed `replayOk=44,
replayFail=1` (≈2%). This was **not** a frame failing to reach the bus.

`canTxTwaiTransmit()` takes the TX barrier mutex non-blockingly
(`xSemaphoreTake(mutex, 0)`). When replay collides with another injector
(strobe / AP high-beam / service mode) holding the barrier, it returns
`ESP_ERR_TIMEOUT` (`0x107`). The old code counted that as a replay failure.

Fix in `feifanEapNrTick()`:

```c
const esp_err_t err = canTxTwaiTransmit(&out, epoch);
eapNrLastErr = (int16_t)err;
if (err == ESP_ERR_TIMEOUT) {
    eapNrReplayBusy++;   // yield, not failure
    return;              // do not advance period timestamp or frame counter
}
```

- `ESP_ERR_TIMEOUT` is tracked separately as `replayBusy` (a yield), **not** a
  failure; it neither advances the throttle timestamp nor the burst counter,
  so the frame is retried on the next 100 ms tick.
- `lastErr` widened from `int8_t` to `int16_t`: `0x107` was previously
  truncated to `0x07`, hiding the real error.
- Status line now shows a yield count and an error name (`TX-busy` /
  `gate-closed` / `send-failed`) instead of a truncated number.

---

## Observability

`status.eapNr` exposes:

```
enabled, prereq, banned, restore,
cacheValid, cacheLen, cacheHex,
stockRx, stockMs,
replayOk, replayFail, replayBusy, replaySkip,
burst, lastMs, lastErr, lastErrName, epoch
```

The UI status row reads one of: **Off** / **Missing prerequisite** / **Waiting
for first frame** / **Car is sending it** / **Replayed N frames**, plus the
cached payload hex. `prereq` tells the user which of the two prerequisite
switches is still missing.

---

## Verification

1. Build: Arduino-ESP32 core 3.3.12, FQBN
   `esp32:esp32:esp32p4:ChipVariant=postv3,FlashSize=32M,PartitionScheme=app5M_fat24M_32MB,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio`.
   Compile exits 0 with 0 warnings; app image 1,486,224 B (31% of the 4.5 MB
   OTA partition).
2. Page integrity: the packer byte-compares the gzip page embedded in the
   image against `dashboard_source.html` (533,287 B, sha256
   `d02c61f4…`) — confirms the UI change actually landed in the binary.
3. JS syntax: all `<script>` blocks pass `node --check`.
4. On-car: with both prerequisites on, observe `status.eapNr` over a cold
   power cycle:
   - first cycle → `cacheValid` becomes true once a stock `0x331` is seen;
   - subsequent cycles → `replayOk` climbs during the 60 s window, `burst`
     stops at ≤600, and `stockRx`/`replayBusy` explain any yields;
   - confirm the touchscreen no longer needs a manual reboot, and TLSSC is
     present after boot.
5. Regression: disable either prerequisite → `enabled` stays but `prereq=false`,
   `replayOk` does not increase; first use with no cache → `replaySkip`
   increases, no frame emitted.

Note: frames/payload were derived from static reverse engineering (evidence
grade A) and have **not** been cross-checked with a real-car CAN capture. The
author should validate on-car before merge.

---

## Guarantees and limits

**Guaranteed:** if this controller's CAN comes up before the MCU reads its DAS
config, the MCU reads a `SELF_DRIVING` `0x331` and needs no manual reboot. If the
MCU reads later, the cached frame stays on the bus for the whole window, so a
later re-evaluation sees it too.

**Not guaranteed:** if the car broadcasts `0x331` continuously (we yield and
fall back to read-modify-retransmit) *and* the MCU finished reading before our
CAN was up, that one power-up still needs a manual reboot. This is bus timing,
not something software can undo. Judge the case from `stockRx`, `replayOk`, and
`cacheHex`.

---

## Files changed (core patch)

```
firmware/P4can-unlock-more/feifan_eap_norestart.h   (new, ~245 lines)
firmware/P4can-unlock-more/can_runtime.h            (+2 hooks)
firmware/P4can-unlock-more/feifan_api.h             (status + action)
```

Project-specific UI/doc/sim files (`dashboard_source.html`, `index_html.h`,
`sim/p4can-sim.html`, `tools/more/*`, `README*`, `CHANGELOG.md`) carry the
switch and documentation and are included in the full branch but are not
required by the core logic.

---

## Safety / scope

- Standard Model 3/Y only; Model Y L remains unsupported until its `0x331`
  field semantics are independently verified (same restriction as the stock
  `doInjectTlsscRestore()`).
- Closed-course debugging tool. Not affiliated with Tesla; Tesla and Model 3/Y
  are trademarks of their owners.
- The patch only transmits when the user explicitly enables the switch and
  both prerequisites hold; the default is no change in behavior.

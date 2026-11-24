# P4can-No-Reboot

> One small thing, fixed: with **"Banned vehicle + TLSSC restore"** enabled, the
> car no longer needs a manual MCU reboot on every power cycle.

A ready-to-flash Tesla CAN toolkit firmware for the **Waveshare ESP32-P4 WiFi6
3×TWAI** board (32 MB flash), based on
[P4can-universal-unlock](https://github.com/06066060606060/P4can-universal-unlock)
`v3.29.3`. The root holds the **compiled firmware** (`.bin`); the complete
source tree is in the [`source/`](source/) subdirectory.

**UI defaults to Chinese**; the language switch in the top-right toggles English.

> [!WARNING]
> **32 MB flash only** (Waveshare ESP32-P4-WIFI6 / rev3.2). The
> `app5M_fat24M_32MB` partition table does not fit a 16 MB board — flashing this
> image on 16 MB **bricks it**, and you will need USB to recover.

---

## The bug it fixes

**Symptom.** With "Banned vehicle + TLSSC restore" on, the MCU had to be rebooted
by hand every power cycle to get TLSSC back.

**Cause.** TLSSC restore only did read-modify-rewrite of `0x331` — the car sends a
frame, we alter one. But the MCU reads and latches its DAS config **once, at its own
boot**. If no modified `0x331` is on the bus at that instant, it latches "banned",
and only a manual MCU reboot makes it re-read. Upstream Flipper docs call this out
directly as "Triggers MCU reboot".

**Fix** — move the patch ahead of the MCU's boot-time read:

| Step | What it does |
|---|---|
| Cache | store the patched `0x331` payload in NVS (at most one write per power cycle, to protect flash) |
| Replay | on CAN B ready, open a 60 s window and replay the cached frame at 10 Hz, without waiting for the car |
| Yield | if the car starts sending `0x331` itself (fresh frame within 2 s), stop immediately and fall back to read-modify-rewrite |
| Finish | stop for good when the window expires or 600 frames are sent — never replay indefinitely |

It only acts when **both** "Banned vehicle" and "TLSSC restore" are on; without a
cached frame it sends nothing; and it goes through the same TX barrier / admin
suspend / USB-passive / epoch gates as every other injection.

A follow-up fix handled the occasional "45 frames replayed · 1 failed": that
failure was not a frame failing to go out, but a non-blocking TX-barrier lock
collision with strobe/AP injection returning `ESP_ERR_TIMEOUT`. It is now counted
as a yield (not a failure) and retried on the next tick; the error code was widened
from `int8_t` to `int16_t` so it no longer truncates.

**How to check.** The status line under "More features → EAP" reads one of: off /
missing prerequisite / waiting for first frame / car is sending it / replayed N
frames. `status.eapNr` exposes `enabled`, `prereq`, `cacheValid`, `cacheHex`,
`stockRx`, `replayOk`, `replayFail`, `burst`, `lastErr`.

**Guarantee.** If our CAN comes up before the MCU reads its config, it sees the
`SELF_DRIVING` `0x331` and needs no manual reboot. If the car periodically sends
its own `0x331`, we yield — and if the MCU read before our CAN was up, that one
cycle still needs a manual reboot. That is bus timing, not something software can
recover. Judge the real car by `stockRx` / `replayOk` / `cacheHex`.

**Also included** (incidental to the base firmware): lighting page (author's
v3.35.6, four entries), More features page, browser OTA update, Wi-Fi relay,
packet capture, bilingual UI.

---

## Source code

The complete source tree is in [`source/`](source/), matching the `*-nr1-*.bin`
images at the repository root (v3.29.3 + EAP anti-restart + the full lighting
page and other changes).

- Firmware sketch: `source/firmware/P4can-unlock-more/P4can-unlock-more.ino`
- Dashboard: `source/firmware/P4can-unlock-more/dashboard_source.html`
- Build/packaging scripts: `source/tools/more/` (`pack_ota.py`, `build_sim.py`, …)
- EAP anti-restart: `source/firmware/P4can-unlock-more/feifan_eap_norestart.h`
- History: `source/CHANGELOG.md`

Source is released under the same terms as upstream; see the root
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). The lighting algorithms are derived
from the author's v3.35.6 reference implementation, with attribution noted in
the source comments and NOTICE. This branch carries the full feature set; for
the minimal EAP-only fix sent upstream see
[upstream PR #15](https://github.com/06066060606060/P4can-universal-unlock/pull/15).

## Download

| File | Size | Use |
|---|---|---|
| `P4can-unlock-more-v3.29.3-nr1-ota.bin` | 1,486,224 B (1.42 MB) | **Everyday updates.** App partition only, rollback on failure |
| `P4can-unlock-more-v3.29.3-nr1-full.bin` | 33,554,432 B (32 MB) | **First flash / recovery.** Full image with bootloader and partition table |
| `manifest.json` | 1.3 KB | sha256, sizes, partition headroom, build parameters |

Checksums (from `manifest.json`, verified):

| File | sha256 |
|---|---|
| OTA | `309e5090883223c1a4a55f171b5e805f00eedec1582093f69760fb32b5e02c11` |
| full | `ee1906034b35b1621b31ca3c47a0c1d6e8fe7863e8afe05286ae77fcfbe8f1f9` |

Verify after download:

```bash
sha256sum P4can-unlock-more-v3.29.3-nr1-ota.bin
# expect: 309e5090...02c11
```

---

## Flashing

### `*-ota.bin` — everyday updates

1. Join the P4's hotspot on phone or PC
2. Open `http://192.168.4.1` → **Settings → Firmware update**
3. Pick `P4can-unlock-more-v3.29.3-nr1-ota.bin`
4. Tap once, confirm, wait for the progress bar to finish and the device to reboot

OTA only writes the app partition — **it cannot change the bootloader, partition
table or flash parameters**.

### `*-full.bin` — first flash / recovery

```bash
esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash 0x0 P4can-unlock-more-v3.29.3-nr1-full.bin
```

Full image layout (ESP32-P4 differs from S3; `0x0` is blank `0xFF`):

| Offset | Content |
|---|---|
| `0x2000` | bootloader (magic `E9`) |
| `0x8000` | partition table (magic `AA50`) |
| `0x10000` | app image (magic `E9`) |

After flashing, confirm the page header reads `v3.29.3-nr1`.

---

## Hardware

Board: Waveshare ESP32-P4-WIFI6, rev3.2, 32 MB flash, `app5M_fat24M_32MB` scheme.
`ota_0` / `ota_1` are `0x480000` each (4.5 MB); single-image cap 4,718,592 B.

| Bus | TWAI | TX | RX |
|---|---|---|---|
| CAN 1 BODY | 0 | GPIO20 | GPIO21 |
| CAN 2 CHASSIS | 1 | GPIO22 | GPIO23 |
| CAN 3 PARTY | 2 | GPIO26 | GPIO27 |

---

## Known limitations

- `gauge` still returns 503: 0xB0 cluster and 0x5D9 tire-pressure payloads are undecoded.
- EAP anti-restart depends on bus power-up timing; see "Guarantee" above.
- Light strobe / rear fog frame definitions come from static reverse engineering
  (grade A), **no real-car capture comparison**; verify dynamic brake light and
  power/brake-domain changes in a closed area first.
- Partition table, bootloader and flash parameters can only be changed over USB.
- Not every write behavior has been verified on a real car.

---

## Safety

This is a tool for **closed-course debugging**. Changing vehicle behavior
(light strobes, nag suppression, driver-monitor suppression, etc.) may be illegal
on public roads and may put the vehicle outside its type-approved state.

- Use at your own risk; the author accepts no liability
- Do not use light / horn / speed-limit-affecting functions on public roads
- Make sure you have a recovery path (full image + USB cable) before flashing

Not affiliated with Tesla. Tesla, Model 3/Y and other trademarks belong to their
owners. See [NOTICE](NOTICE) for the firmware baseline's terms and [LICENSE](LICENSE)
for the license of this project's additions.

---

## ☕ Support

> The firmware is free. If it saved you time, feel free to buy the author a coffee.

<table>
  <tr>
    <td align="center"><img src="docs/sponsor/web_alipay.jpg" width="220" alt="Alipay"><br><sub><b>Alipay</b></sub></td>
    <td align="center"><img src="docs/sponsor/web_wechatpay.jpg" width="220" alt="WeChat Pay"><br><sub><b>WeChat Pay</b></sub></td>
  </tr>
</table>

## 📮 Contact

> Can't install, no response after flashing, or want a feature — reach out.

<table>
  <tr>
    <td align="center"><img src="docs/sponsor/web_douyin.jpg" width="220" alt="Douyin QR"><br><sub><b>Douyin · @耶基大学懂事长</b><br>ID <code>yejiuni</code></sub></td>
    <td align="center"><img src="docs/sponsor/web_wechatauthor.jpg" width="220" alt="WeChat QR"><br><sub><b>WeChat · scan to add</b></sub></td>
  </tr>
</table>

---

[中文](README.md)

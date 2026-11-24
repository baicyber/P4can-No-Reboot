#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// 上游 Wi-Fi 中继（AP + STA + NAPT）
// ═══════════════════════════════════════════════════════════════════════════
//
// 现象：手机连 P4 热点（192.168.4.1）后就没网了 —— 因为 P4 只是个 AP。
//
// 做法：把 Wi-Fi 切到 AP+STA，STA 连家里的路由器，再用 lwIP 的 NAPT
// （IP 转发 + 端口映射）把 AP 侧客户端的流量转出去。
//
//   · 本机编译用的 sdkconfig 里 CONFIG_LWIP_IP_FORWARD / IPV4_NAPT 都是 y，
//     所以 esp_netif_napt_enable() 可以直接用；
//   · AP 的 DHCP 要把 DNS 改成上游的 DNS，否则客户端「有 IP 但解析不了域名」，
//     看起来还是没网 —— 这一步最容易漏；
//   · 连不上时不阻塞：WiFi.begin() 只是发起，真正的状态由 p4UpLoop() 每秒轮询，
//     失败每 15 s 重试一次，不重启任何总线。
//
// 注意：不要调 WiFi.disconnect(true) / WiFi.mode(WIFI_OFF)。本机 Wi-Fi 是
// esp-hosted（P4 + C6），关掉 Wi-Fi 会把 Hosted 传输一起拆掉，页面上就再也
// 连不回来了。
// ═══════════════════════════════════════════════════════════════════════════

#include <esp_netif.h>
#include <Preferences.h>

#define P4_UP_NS "wifi"
#define P4_UP_SSID "up_ssid"
#define P4_UP_PASS "up_pass"
#define P4_UP_ON "up_on"
#define P4_UP_RETRY_MS 15000u

static String p4UpSsid;
static String p4UpPass;
static bool p4UpEnabled = false;      // 用户打开过中继
static bool p4UpHaveCfg = false;
static bool p4UpPending = false;      // 需要重新发起连接
static bool p4UpConnected = false;
static bool p4UpNapt = false;
static uint32_t p4UpLastTryMs = 0;
static uint32_t p4UpAttempts = 0;
static uint32_t p4UpConnectedMs = 0;
static char p4UpLastErr[40] = "";

static bool p4UplinkWanted() { return p4UpEnabled && p4UpHaveCfg; }

static void p4UpLoad() {
  Preferences p;
  if (!p.begin(P4_UP_NS, true)) return;
  p4UpSsid = p.getString(P4_UP_SSID, "");
  p4UpPass = p.getString(P4_UP_PASS, "");
  p4UpEnabled = p.getBool(P4_UP_ON, false);
  p.end();
  p4UpHaveCfg = p4UpSsid.length() > 0;
  if (p4UplinkWanted()) p4UpPending = true;
}

static bool p4UpSave() {
  Preferences p;
  if (!p.begin(P4_UP_NS, false)) return false;
  const bool ok = p.putString(P4_UP_SSID, p4UpSsid) == p4UpSsid.length() &&
                  p.putString(P4_UP_PASS, p4UpPass) == p4UpPass.length() &&
                  p.putBool(P4_UP_ON, p4UpEnabled);
  p.end();
  return ok;
}

// 打开 / 更新上游：只存配置并把连接排进队列，真正 begin() 在 loop 里做，
// 这样 HTTP 请求不会被扫描+关联的几秒钟卡住。
static bool p4UpApply(const String &ssid, const String &pass) {
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63) return false;
  p4UpSsid = ssid;
  p4UpPass = pass;
  p4UpHaveCfg = true;
  p4UpEnabled = true;
  p4UpPending = true;
  p4UpConnectedMs = 0;
  p4UpLastErr[0] = '\0';
  return p4UpSave();
}

// 关闭中继：断开 STA，把 Wi-Fi 退回纯 AP。不动 AP 本身，页面不会掉线。
static void p4UpDisable() {
  p4UpEnabled = false;
  p4UpPending = false;
  p4UpConnected = false;
  p4UpNapt = false;
  p4UpConnectedMs = 0;
  p4UpSave();
  WiFi.disconnect(false, false);
  if (WiFi.getMode() != WIFI_AP) WiFi.mode(WIFI_AP);
}

// AP 的 DHCP 下发上游 DNS。不做这一步，客户端能 ping IP 却解析不了域名。
static void p4UpSyncDns() {
  esp_netif_t *sta = WiFi.STA.netif();
  esp_netif_t *ap = WiFi.AP.netif();
  if (!sta || !ap) return;
  esp_netif_dns_info_t dns = {};
  if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
      dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
    dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = htonl(0x08080808u);  // 8.8.8.8 兜底
  }
  esp_netif_dhcps_stop(ap);
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                         &dns.ip.u_addr.ip4, sizeof(esp_ip4_addr_t));
  esp_netif_dhcps_start(ap);
}

static bool p4UpEnableNapt() {
  esp_netif_t *ap = WiFi.AP.netif();
  if (!ap) return false;
  return esp_netif_napt_enable(ap) == ESP_OK;
}

// 每秒调一次（loop()）。
static void p4UpLoop() {
  const uint32_t now = (uint32_t)millis();

  if (!p4UplinkWanted()) {
    p4UpConnected = false;
    return;
  }

  if (p4UpPending) {
    p4UpPending = false;
    p4UpAttempts++;
    p4UpLastTryMs = now;
    if (WiFi.getMode() != WIFI_AP_STA) {
      if (!WiFi.mode(WIFI_AP_STA)) {
        snprintf(p4UpLastErr, sizeof(p4UpLastErr), "mode-failed");
        return;
      }
    }
    WiFi.begin(p4UpSsid.c_str(), p4UpPass.c_str());
    return;
  }

  const bool linked = (WiFi.status() == WL_CONNECTED);
  if (linked) {
    if (!p4UpConnected) {
      p4UpConnected = true;
      p4UpConnectedMs = now;
      p4UpLastErr[0] = '\0';
      // 先开转发，再改 DHCP 的 DNS：顺序反了会让已连上的客户端拿到旧 DNS。
      p4UpNapt = p4UpEnableNapt();
      p4UpSyncDns();
    } else if (!p4UpNapt) {
      p4UpNapt = p4UpEnableNapt();
    }
    return;
  }

  p4UpConnected = false;
  p4UpNapt = false;
  if ((uint32_t)(now - p4UpLastTryMs) >= P4_UP_RETRY_MS) {
    p4UpAttempts++;
    p4UpLastTryMs = now;
    WiFi.begin(p4UpSsid.c_str(), p4UpPass.c_str());
  }
}

static String p4UpStatusJson() {
  String j = "{";
  j += "\"enabled\":"; j += p4UpEnabled ? "true" : "false";
  j += ",\"haveCfg\":"; j += p4UpHaveCfg ? "true" : "false";
  j += ",\"ssid\":\""; j += p4UpSsid; j += "\"";
  j += ",\"connected\":"; j += p4UpConnected ? "true" : "false";
  j += ",\"napt\":"; j += p4UpNapt ? "true" : "false";
  j += ",\"ip\":\""; j += p4UpConnected ? WiFi.localIP().toString() : String(""); j += "\"";
  j += ",\"rssi\":"; j += p4UpConnected ? String((int)WiFi.RSSI()) : String("0");
  j += ",\"attempts\":"; j += (unsigned long)p4UpAttempts;
  j += ",\"upSec\":"; j += p4UpConnected ? (unsigned long)((millis() - p4UpConnectedMs) / 1000UL) : 0UL;
  j += ",\"stations\":"; j += String((int)WiFi.softAPgetStationNum());
  j += ",\"error\":\""; j += p4UpLastErr; j += "\"";
  j += "}";
  return j;
}

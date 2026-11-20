#pragma once

// ====================== USER CONFIGURATION ======================

// Your WiFi credentials. Leave SSID empty to force AP setup mode
// (device then broadcasts "WOL-Setup" so you can reach it at http://192.168.4.1).
#define WIFI_SSID     ""
#define WIFI_PASS     ""

// mDNS hostname: http://wol.local
#define MDNS_NAME     "wol"

// Web server port
#define WEB_PORT      80

// Fallback AP mode
#define AP_SSID       "WOL-Setup"
#define AP_PASS       "setup-wol-123"   // min 8 chars; only used before WiFi is configured

// First-boot credentials (stored hashed in LittleFS on first boot).
// CHANGE THIS PASSWORD after first login (Settings section in the UI).
#define DEFAULT_USER  "admin"
#define DEFAULT_PASS  "changeme-123"

// ====================== SECURITY PARAMETERS ======================

#define SESSION_TTL_MS       (3600UL * 1000UL)  // session = 1 hour
#define SESSION_TOKEN_BYTES  32                 // 256-bit session token
#define COOKIE_NAME          "WOLSID"

#define MAX_SESSIONS         4                  // live sessions before oldest is evicted

#define MAX_LOGIN_ATTEMPTS   5                  // failures per IP before lockout
#define LOGIN_WINDOW_MS      (10UL * 60UL * 1000UL)
#define LOCKOUT_MS           (60UL * 1000UL)

#define MAX_PASSWORD_LEN     64

// ====================== BEHAVIOUR ======================

#define MAX_MACHINES         12

// Awake probing: each machine with a "check port" > 0 is probed every round.
// The probe is a short TCP connect; set CHECK_TIMEOUT_MS high enough for
// your LAN (WAN hosts need more).
#define CHECK_ROUND_INTERVAL_MS  (15UL * 1000UL)
#define CHECK_TIMEOUT_MS         400

// After a magic packet we assume "waking" for this long (BIOS/POST is slow).
#define WAKE_GRACE_MS        (120UL * 1000UL)

#define MACHINES_FILE        "/machines.json"
#define AUTH_FILE            "/auth.json"

/*
 * ESP8266 Wake-on-LAN Web Controller
 *
 * A single-page dashboard behind a session-based login. Authenticate, see
 * which machines are awake, send magic packets, add/edit/delete machines.
 *
 * Security model (plain-HTTP LAN device, so this is "as good as it gets"
 * without TLS on an ESP8266):
 *   - Credentials stored as salted SHA-256 (salt is random per device)
 *     in LittleFS, never as plaintext.
 *   - 256-bit random session token in an HttpOnly, SameSite=Strict cookie;
 *     tokens live in RAM only and expire after SESSION_TTL_MS.
 *   - Per-session CSRF token; every state-changing POST must carry it in
 *     the X-CSRF-Token header (custom headers cannot be sent cross-origin
 *     without a CORS preflight, which we never answer).
 *   - Login rate limiting per IP: MAX_LOGIN_ATTEMPTS failures within
 *     LOGIN_WINDOW_MS -> LOCKOUT_MS lockout.
 *   - Constant-time token comparison to avoid timing oracles.
 *   - No cache headers on API or page responses.
 *
 * If you need real secrecy of the password over the wire, front this with
 * an HTTPS reverse proxy or reach it over VPN/WireGuard only.
 *
 * Libraries: ESP8266 core (WiFi, WebServer, LittleFS, BearSSL),
 *            ArduinoJson v7.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <BearSSLHelpers.h>
#include <esp8266_peri.h>

#include "config.h"
#include "web_page.h"

ESP8266WebServer server(WEB_PORT);
WiFiUDP           wolUdp;
WiFiClient        probeClient;

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct Machine {
  String         id;
  String         name;
  String         mac;          // normalized "AA:BB:CC:DD:EE:FF"
  String         ip;           // may be empty
  uint16_t       wolPort   = 9;
  uint16_t       checkPort = 0;
  bool           awake     = false;
  bool           checked   = false;
  unsigned long  lastCheck = 0;     // millis() of last probe
  unsigned long  lastWake  = 0;     // millis() of last magic packet
};

struct DeviceAuth {
  String user;
  String salt;    // hex, 16 bytes
  String hash;    // hex, sha256(salt + password)
};

struct WebSession {
  bool          used     = false;
  uint32_t      created  = 0;
  char          token[2 * SESSION_TOKEN_BYTES + 1];
  char          csrf[2 * SESSION_TOKEN_BYTES + 1];
};

struct RateLimit {
  IPAddress     ip;
  uint8_t       fails       = 0;
  uint32_t      windowStart = 0;
  uint32_t      lockUntil   = 0;
};

static std::vector<Machine> gMachines;
static DeviceAuth           gAuth;
static WebSession           gSessions[MAX_SESSIONS];
static RateLimit            gRate;

#define WIFI_FILE "/wifi.json"

struct WifiConfig {
  String ssid;
  String pass;    // must be recoverable to connect, stored in /wifi.json
  bool   dhcp     = true;
  String ip;
  String gateway;
  String netmask;
  String dns;
};

static WifiConfig        gWifi;
static bool              gRebootPending = false;
static uint32_t          gRebootAt      = 0;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

String randomHex(size_t nbytes) {
  static const char hexc[] = "0123456789abcdef";
  String out;
  out.reserve(nbytes * 2);
  for (size_t i = 0; i < nbytes; i += 4) {
    uint32_t r = RANDOM_REG32;
    for (size_t j = 0; j < 4 && i + j < nbytes; j++) {
      unsigned b = (r >> (8 * j)) & 0xFF;
      out += hexc[b >> 4];
      out += hexc[b & 0xF];
    }
  }
  return out;
}

String sha256Hex(const String &input) {
  BearSSL::HashSHA256 h;
  h.begin();
  h.add(input.c_str(), input.length());
  h.end();
  const uint8_t *d = (const uint8_t *)h.hash();
  static const char hexc[] = "0123456789abcdef";
  String out;
  out.reserve(64);
  for (size_t i = 0; i < 32; i++) {
    out += hexc[d[i] >> 4];
    out += hexc[d[i] & 0xF];
  }
  return out;
}

// Constant-time comparison (no early exit on mismatch).
bool ctEquals(const String &a, const char *b) {
  size_t lb = strlen(b);
  if (a.length() != lb) return false;
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < lb; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Accepts AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF, AABBCCDDEEFF, aabbccddee00
bool parseMac(const String &in, uint8_t out[6]) {
  String hex;
  hex.reserve(12);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isxdigit((unsigned char)c)) {
      hex += (char)toupper((unsigned char)c);
    } else if (c == ':' || c == '-' || c == ' ' || c == '.') {
      continue;
    } else {
      return false;
    }
  }
  if (hex.length() != 12) return false;
  for (int i = 0; i < 6; i++) {
    uint8_t v = 0;
    for (int j = 0; j < 2; j++) {
      char c = hex[i * 2 + j];
      v = (uint8_t)(v * 16 + (c <= '9' ? c - '0' : c - 'A' + 10));
    }
    out[i] = v;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Persistence (LittleFS)
// ---------------------------------------------------------------------------

bool loadAuth() {
  if (!LittleFS.exists(AUTH_FILE)) return false;
  File f = LittleFS.open(AUTH_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  bool ok = deserializeJson(doc, f) == DeserializationError::Ok;
  if (ok) {
    gAuth.user = doc["user"].as<String>();
    gAuth.salt = doc["salt"].as<String>();
    gAuth.hash = doc["hash"].as<String>();
    ok = gAuth.user.length() > 0 && gAuth.salt.length() == 32 && gAuth.hash.length() == 64;
  }
  f.close();
  return ok;
}

void saveAuth() {
  JsonDocument doc;
  doc["user"] = gAuth.user;
  doc["salt"] = gAuth.salt;
  doc["hash"] = gAuth.hash;
  File f = LittleFS.open(AUTH_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void ensureAuth() {
  if (loadAuth()) return;
  gAuth.user = DEFAULT_USER;
  gAuth.salt = randomHex(16);
  gAuth.hash = sha256Hex(gAuth.salt + DEFAULT_PASS);
  saveAuth();
  Serial.println(F("[auth] First boot: stored default credentials (hashed)."));
  Serial.println(F("[auth] CHANGE THE PASSWORD via the dashboard 'Change credentials'."));
}

bool loadMachines() {
  if (!LittleFS.exists(MACHINES_FILE)) return true;
  File f = LittleFS.open(MACHINES_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  bool ok = deserializeJson(doc, f) == DeserializationError::Ok;
  gMachines.clear();
  if (ok && doc.is<JsonArray>()) {
    for (JsonObject o : doc.as<JsonArray>()) {
      Machine m;
      m.id        = o["id"].as<String>();
      m.name      = o["name"].as<String>();
      m.mac       = o["mac"].as<String>();
      m.ip        = o["ip"].as<String>();
      m.wolPort   = o["wolPort"]   | 9;
      m.checkPort = o["checkPort"] | 0;
      if (m.id.length() && m.name.length() && m.mac.length()) gMachines.push_back(m);
    }
  } else {
    ok = false;
  }
  f.close();
  return ok;
}

void saveMachines() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto &m : gMachines) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]        = m.id;
    o["name"]      = m.name;
    o["mac"]       = m.mac;
    o["ip"]        = m.ip;
    o["wolPort"]   = m.wolPort;
    o["checkPort"] = m.checkPort;
  }
  File f = LittleFS.open(MACHINES_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

int findMachineIndex(const String &id) {
  for (size_t i = 0; i < gMachines.size(); i++)
    if (gMachines[i].id == id) return (int)i;
  return -1;
}

bool loadWifiConfig() {
  if (!LittleFS.exists(WIFI_FILE)) return false;
  File f = LittleFS.open(WIFI_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  bool ok = deserializeJson(doc, f) == DeserializationError::Ok;
  f.close();
  if (!ok) return false;
  gWifi.ssid    = doc["ssid"] | "";
  gWifi.pass    = doc["pass"] | "";
  gWifi.dhcp    = doc["dhcp"] | true;
  gWifi.ip      = doc["ip"] | "";
  gWifi.gateway = doc["gateway"] | "";
  gWifi.netmask = doc["netmask"] | "";
  gWifi.dns     = doc["dns"] | "";
  return gWifi.ssid.length() > 0;
}

void saveWifiConfig() {
  JsonDocument doc;
  doc["ssid"]    = gWifi.ssid;
  doc["pass"]    = gWifi.pass;
  doc["dhcp"]    = gWifi.dhcp;
  doc["ip"]      = gWifi.ip;
  doc["gateway"] = gWifi.gateway;
  doc["netmask"] = gWifi.netmask;
  doc["dns"]     = gWifi.dns;
  File f = LittleFS.open(WIFI_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

// ---------------------------------------------------------------------------
// Sessions / auth plumbing
// ---------------------------------------------------------------------------

WebSession *findSession(const String &token) {
  if (token.length() != 2 * SESSION_TOKEN_BYTES) return nullptr;
  uint32_t now = millis();
  for (auto &s : gSessions) {
    if (s.used && ctEquals(token, s.token) && now - s.created <= SESSION_TTL_MS) {
      return &s;
    }
  }
  return nullptr;
}

WebSession *createSession() {
  uint32_t oldest = 0xFFFFFFFFUL;
  WebSession *slot = &gSessions[0];
  for (auto &s : gSessions) {
    if (!s.used) { slot = &s; break; }
    if (s.created < oldest) { oldest = s.created; slot = &s; }
  }
  slot->used = true;
  slot->created = millis();
  strlcpy(slot->token, randomHex(SESSION_TOKEN_BYTES).c_str(), sizeof(slot->token));
  strlcpy(slot->csrf,  randomHex(SESSION_TOKEN_BYTES).c_str(), sizeof(slot->csrf));
  return slot;
}

void destroySession(const String &token) {
  for (auto &s : gSessions)
    if (s.used && ctEquals(token, s.token)) s.used = false;
}

String getCookie(const char *name) {
  String header = server.header("Cookie");
  if (!header.length() && server.hasHeader("cookie")) header = server.header("cookie");
  if (!header.length()) return "";
  String want = String(name) + "=";
  int start = header.indexOf(want);
  if (start < 0) return "";
  start += want.length();
  int end = start;
  while (end < (int)header.length() && header[end] != ';' && header[end] != ' ') end++;
  return header.substring(start, end);
}

// Returns the live session for this request, or nullptr.
WebSession *currentSession() {
  String tok = getCookie(COOKIE_NAME);
  if (!tok.length()) return nullptr;
  return findSession(tok);
}

void sendJson(int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

void sendError(int code, const String &msg) {
  JsonDocument doc;
  doc["error"] = msg;
  sendJson(code, doc);
}

bool rateLimited() {
  IPAddress ip = server.client().remoteIP();
  uint32_t now = millis();
  if (gRate.ip != ip) {
    gRate = RateLimit();
    gRate.ip = ip;
    return false;
  }
  if (gRate.fails >= MAX_LOGIN_ATTEMPTS) {
    if (now < gRate.lockUntil) return true;
    gRate.fails = 0;
  }
  return false;
}

void recordLoginFailure() {
  uint32_t now = millis();
  if (gRate.fails == 0 || now - gRate.windowStart > LOGIN_WINDOW_MS) {
    gRate.windowStart = now;
    gRate.fails = 0;
  }
  gRate.fails++;
  if (gRate.fails >= MAX_LOGIN_ATTEMPTS) gRate.lockUntil = now + LOCKOUT_MS;
}

bool verifyPassword(const String &password) {
  return ctEquals(sha256Hex(gAuth.salt + password), gAuth.hash.c_str());
}

// Guards: session on all /api routes, CSRF additionally on POSTs.
bool guardAuth(bool wantCsrf) {
  WebSession *s = currentSession();
  if (!s) {
    sendError(401, "authentication required");
    return false;
  }
  if (wantCsrf) {
    String hdr = server.header("X-CSRF-Token");
    if (!hdr.length() && server.hasHeader("x-csrf-token")) hdr = server.header("x-csrf-token");
    if (!hdr.length() || !ctEquals(hdr, s->csrf)) {
      sendError(403, "CSRF token missing or invalid");
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Awake probing (round-robin, short timeout, cached results)
// ---------------------------------------------------------------------------

size_t        gCursor   = 0;

// Short TCP-connect probe. WiFiClient.connect() honors the millisecond
// _timeout set below, so a dead host costs at most CHECK_TIMEOUT_MS of
// blocking per machine per round.
void probeStart(int idx) {
  Machine &m = gMachines[idx];
  IPAddress ip;
  if (!m.ip.length() || !ip.fromString(m.ip)) {
    m.checked = false;
    return;
  }
  probeClient.setTimeout(CHECK_TIMEOUT_MS);
  bool ok = probeClient.connect(ip, m.checkPort) != 0;
  m.checked = true;
  m.awake = ok;
  m.lastCheck = millis();
  probeClient.stop();
}

void serviceChecks() {
  if (WiFi.status() != WL_CONNECTED) return;
  size_t n = gMachines.size();
  if (n == 0) return;
  unsigned long now = millis();
  for (size_t i = 0; i < n; i++) {
    size_t idx = (gCursor + i) % n;
    Machine &m = gMachines[idx];
    if (m.checkPort == 0 || !m.ip.length()) continue;
    if (m.checked && now - m.lastCheck < CHECK_ROUND_INTERVAL_MS) continue;
    gCursor = (idx + 1) % n;
    probeStart((int)idx);
    return;
  }
}

// ---------------------------------------------------------------------------
// Wake-on-LAN
// ---------------------------------------------------------------------------

void sendWolPacket(const IPAddress &dst, const uint8_t mac[6], uint16_t port) {
  uint8_t pkt[6 + 16 * 6];
  memset(pkt, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, mac, 6);
  wolUdp.beginPacket(dst, port);
  wolUdp.write(pkt, sizeof(pkt));
  wolUdp.endPacket();
}

void wakeMachine(Machine &m) {
  uint8_t mac[6];
  if (!parseMac(m.mac, mac)) return;
  uint32_t local = (uint32_t)WiFi.localIP();
  uint32_t mask  = (uint32_t)WiFi.subnetMask();
  IPAddress bcast((local & mask) | (~mask));
  IPAddress unicast;
  bool hasIp = m.ip.length() && unicast.fromString(m.ip);
  for (int rep = 0; rep < 3; rep++) {
    sendWolPacket(bcast, mac, m.wolPort);
    if (hasIp) sendWolPacket(unicast, mac, m.wolPort);
    delay(10);
  }
  m.lastWake = millis();
  Serial.printf("[wol] magic packet sent: %s (%s)\n", m.name.c_str(), m.mac.c_str());
}

// ---------------------------------------------------------------------------
// API handlers
// ---------------------------------------------------------------------------

bool readJsonBody(JsonDocument &req) {
  if (!server.hasArg("plain")) return false;
  return deserializeJson(req, server.arg("plain")) == DeserializationError::Ok;
}

void handleSession() {
  JsonDocument doc;
  WebSession *s = currentSession();
  if (s) {
    doc["authenticated"] = true;
    doc["user"] = gAuth.user;
    doc["csrf"] = s->csrf;
  } else {
    doc["authenticated"] = false;
  }
  sendJson(200, doc);
}

void handleLogin() {
  JsonDocument req;
  if (!readJsonBody(req)) { sendError(400, "bad request"); return; }
  String user = req["user"] | "";
  String pass = req["password"] | "";
  user.trim();

  if (rateLimited()) {
    JsonDocument doc;
    doc["error"] = "Too many failed attempts. Try again later.";
    doc["lockedOut"] = true;
    sendJson(429, doc);
    return;
  }

  if (user.length() < 1 || user.length() > MAX_PASSWORD_LEN ||
      pass.length() < 1 || pass.length() > MAX_PASSWORD_LEN ||
      user != gAuth.user || !verifyPassword(pass)) {
    recordLoginFailure();
    uint8_t left = MAX_LOGIN_ATTEMPTS > gRate.fails ? MAX_LOGIN_ATTEMPTS - gRate.fails : 0;
    JsonDocument doc;
    doc["error"] = "Invalid credentials.";
    doc["attemptsLeft"] = left;
    sendJson(401, doc);
    return;
  }

  gRate = RateLimit();
  gRate.ip = server.client().remoteIP();

  WebSession *s = createSession();
  server.sendHeader("Set-Cookie",
      String(COOKIE_NAME) + "=" + s->token +
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + String(SESSION_TTL_MS / 1000UL));
  JsonDocument doc;
  doc["ok"] = true;
  doc["user"] = gAuth.user;
  doc["csrf"] = s->csrf;
  sendJson(200, doc);
}

void handleLogout() {
  if (!guardAuth(true)) return;
  destroySession(getCookie(COOKIE_NAME));
  server.sendHeader("Set-Cookie",
      String(COOKIE_NAME) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

void handleMachinesGet() {
  if (!guardAuth(false)) return;
  JsonDocument doc;
  JsonArray arr = doc["machines"].to<JsonArray>();
  unsigned long now = millis();
  for (auto &m : gMachines) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = m.id;
    o["name"] = m.name;
    o["mac"] = m.mac;
    o["ip"] = m.ip;
    o["wolPort"] = m.wolPort;
    o["checkPort"] = m.checkPort;
    o["checked"] = m.checked;
    o["lastCheckAgo"] = m.checked ? (unsigned long)((now - m.lastCheck) / 1000UL) : 0UL;
    unsigned long sinceWake = m.lastWake ? now - m.lastWake : 0;
    const char *status;
    if (!m.checked)         status = "unknown";
    else if (m.awake)       status = "awake";
    else if (sinceWake && sinceWake < WAKE_GRACE_MS) status = "waking";
    else                    status = "asleep";
    o["status"] = status;
  }
  sendJson(200, doc);
}

void handleMachineSave() {
  if (!guardAuth(true)) return;
  JsonDocument req;
  if (!readJsonBody(req)) { sendError(400, "bad request"); return; }

  String id   = req["id"] | "";
  String name = req["name"] | "";
  String mac  = req["mac"] | "";
  String ip   = req["ip"] | "";
  int wolP    = req["wolPort"] | 9;
  int chkP    = req["checkPort"] | 0;

  uint8_t macb[6];
  name.trim();
  mac.trim();
  ip.trim();

  if (name.length() == 0 || name.length() > 32) {
    sendError(400, "Name must be 1-32 characters.");
    return;
  }
  if (!parseMac(mac, macb)) {
    sendError(400, "MAC must be 12 hex digits, e.g. AA:BB:CC:DD:EE:FF");
    return;
  }
  if (ip.length()) {
    IPAddress tmp;
    if (tmp.fromString(ip)) ip = tmp.toString();
    else { sendError(400, "IP address is not valid."); return; }
  }
  if (wolP < 1 || wolP > 65535 || chkP < 0 || chkP > 65535) {
    sendError(400, "Port out of range.");
    return;
  }

  String macNorm = macToString(macb);
  int existing = id.length() ? findMachineIndex(id) : -1;

  if (existing >= 0) {
    Machine &m = gMachines[existing];
    if (ip != m.ip || chkP != m.checkPort) { m.checked = false; m.awake = false; }
    m.name = name;
    m.mac = macNorm;
    m.ip = ip;
    m.wolPort = (uint16_t)wolP;
    m.checkPort = (uint16_t)chkP;
  } else {
    if (gMachines.size() >= MAX_MACHINES) {
      sendError(400, "Machine limit reached (" + String(MAX_MACHINES) + ").");
      return;
    }
    Machine m;
    m.id = randomHex(4);
    m.name = name;
    m.mac = macNorm;
    m.ip = ip;
    m.wolPort = (uint16_t)wolP;
    m.checkPort = (uint16_t)chkP;
    gMachines.push_back(m);
  }
  saveMachines();
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

void handleMachineDelete() {
  if (!guardAuth(true)) return;
  JsonDocument req;
  if (!readJsonBody(req)) { sendError(400, "bad request"); return; }
  String id = req["id"] | "";
  int idx = findMachineIndex(id);
  if (idx < 0) { sendError(404, "Machine not found."); return; }
  gMachines.erase(gMachines.begin() + idx);
  saveMachines();
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

void handleWake() {
  if (!guardAuth(true)) return;
  JsonDocument req;
  if (!readJsonBody(req)) { sendError(400, "bad request"); return; }
  String id = req["id"] | "";
  int idx = findMachineIndex(id);
  if (idx < 0) { sendError(404, "Machine not found."); return; }
  Machine &m = gMachines[idx];
  wakeMachine(m);
  JsonDocument doc;
  doc["ok"] = true;
  doc["name"] = m.name;
  doc["status"] = "waking";
  sendJson(200, doc);
}

void handlePassword() {
  if (!guardAuth(true)) return;
  JsonDocument req;
  if (!readJsonBody(req)) { sendError(400, "bad request"); return; }

  String current  = req["current"] | "";
  String newUser  = req["user"] | "";
  String password = req["password"] | "";
  newUser.trim();

  if (!verifyPassword(current)) { sendError(401, "Current password is wrong."); return; }
  if (password.length() < 8 || password.length() > MAX_PASSWORD_LEN) {
    sendError(400, "New password must be 8-64 characters.");
    return;
  }
  if (newUser.length()) {
    if (newUser.length() > MAX_PASSWORD_LEN) { sendError(400, "Username too long."); return; }
    gAuth.user = newUser;
  }
  gAuth.salt = randomHex(16);
  gAuth.hash = sha256Hex(gAuth.salt + password);
  saveAuth();

  for (auto &s : gSessions) s.used = false;

  WebSession *s = createSession();
  server.sendHeader("Set-Cookie",
      String(COOKIE_NAME) + "=" + s->token +
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + String(SESSION_TTL_MS / 1000UL));
  JsonDocument doc;
  doc["ok"] = true;
  doc["user"] = gAuth.user;
  doc["csrf"] = s->csrf;
  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// WiFi configuration API
// ---------------------------------------------------------------------------

const char *wifiModeText() {
  uint8_t m = WiFi.getMode();
  if ((m & WIFI_STA) && !(m & WIFI_AP)) return "sta";
  if ((m & WIFI_AP)  && !(m & WIFI_STA)) return "ap";
  return "mixed";
}

void handleWifiGet() {
  if (!guardAuth(false)) return;
  JsonDocument doc;
  doc["mode"]      = wifiModeText();
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["address"]   = (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED
                         ? WiFi.localIP().toString()
                         : WiFi.softAPIP().toString();
  doc["ssid"]      = gWifi.ssid;
  doc["dhcp"]      = gWifi.dhcp;
  doc["ip"]        = gWifi.ip;
  doc["gateway"]   = gWifi.gateway;
  doc["netmask"]   = gWifi.netmask;
  doc["dns"]       = gWifi.dns;
  sendJson(200, doc);
}

// Applies new WiFi settings and schedules a reboot. A failed connection still
// returns to AP setup mode on the next boot, so bad credentials are recoverable.
void handleWifiSet() {
  if (!guardAuth(true)) return;
  JsonDocument req;
  if (!readJsonBody(req)) { sendError(400, "bad request"); return; }

  String ssid    = req["ssid"] | "";
  String pass    = req["password"] | "";
  bool   dhcp    = req["dhcp"] | true;
  String ip      = req["ip"] | "";
  String gateway = req["gateway"] | "";
  String netmask = req["netmask"] | "";
  String dns     = req["dns"] | "";
  ssid.trim();
  IPAddress t;

  if (ssid.length()) gWifi.ssid = ssid;
  if (pass.length()) gWifi.pass = pass;

  if (gWifi.ssid.length() == 0 || gWifi.ssid.length() > 32) {
    sendError(400, "SSID is required (1-32 characters).");
    return;
  }
  if (pass.length() > 64) { sendError(400, "WiFi password too long."); return; }

  if (dhcp) {
    gWifi.ip = gWifi.gateway = gWifi.netmask = gWifi.dns = "";
  } else {
    if (!ip.length() || !t.fromString(ip)) {
      sendError(400, "Static mode requires a valid IP address.");
      return;
    }
    if ((gateway.length() && !t.fromString(gateway)) ||
        (netmask.length() && !t.fromString(netmask)) ||
        (dns.length() && !t.fromString(dns))) {
      sendError(400, "Static network fields are not valid addresses.");
      return;
    }
    gWifi.ip      = ip;
    gWifi.gateway = gateway;
    gWifi.netmask = netmask;
    gWifi.dns     = dns;
  }
  gWifi.dhcp = dhcp;

  saveWifiConfig();

  gRebootPending = true;
  gRebootAt = millis() + 1500;

  JsonDocument doc;
  doc["ok"] = true;
  doc["rebootMs"] = 1500;
  sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

void sendIndexPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleNotFound() {
  if (server.uri().startsWith("/api")) {
    sendError(404, "unknown endpoint");
  } else {
    sendIndexPage();
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setupWiFi() {
  WiFi.persistent(false);
  WiFi.setHostname("wol-esp8266");

  if (!loadWifiConfig()) {
    gWifi.ssid = WIFI_SSID;
    gWifi.pass = WIFI_PASS;
    gWifi.dhcp = true;
  }

  if (gWifi.ssid.length()) {
    WiFi.mode(WIFI_STA);
    bool staticOk = false;
    if (!gWifi.dhcp && gWifi.ip.length()) {
      IPAddress ip, gw, nm, dns;
      ip.fromString(gWifi.ip);
      gw.fromString(gWifi.gateway);
      nm.fromString(gWifi.netmask);
      dns.fromString(gWifi.dns);
      if ((uint32_t)nm == 0) nm = IPAddress(255, 255, 255, 0);
      staticOk = WiFi.config(ip, gw, nm, dns);
    }
    if (!staticOk) WiFi.config(0U, 0U, 0U);
    WiFi.begin(gWifi.ssid, gWifi.pass);
    Serial.printf("[wifi] connecting to \"%s\"", gWifi.ssid.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000UL) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[wifi] connected, IP: %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println(F("[wifi] could not connect, falling back to AP mode"));
    Serial.println(F("[wifi] fix credentials in the dashboard once connected to the AP"));
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[wifi] AP mode: SSID \"%s\" pass \"%s\" -> http://%s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

void setupRoutes() {
  server.collectHeaders(F("Cookie"), F("cookie"),
                        F("X-CSRF-Token"), F("x-csrf-token"));

  server.on("/", HTTP_GET, sendIndexPage);
  server.on("/index.html", HTTP_GET, sendIndexPage);

  server.on("/api/session",  HTTP_GET,  handleSession);
  server.on("/api/login",    HTTP_POST, handleLogin);
  server.on("/api/logout",   HTTP_POST, handleLogout);
  server.on("/api/machines", HTTP_GET,  handleMachinesGet);
  server.on("/api/machines", HTTP_POST, handleMachineSave);
  server.on("/api/machines/delete", HTTP_POST, handleMachineDelete);
  server.on("/api/wake",     HTTP_POST, handleWake);
  server.on("/api/password", HTTP_POST, handlePassword);
  server.on("/api/wifi",     HTTP_GET,  handleWifiGet);
  server.on("/api/wifi",     HTTP_POST, handleWifiSet);

  server.onNotFound(handleNotFound);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== ESP8266 Wake-on-LAN Controller ==="));

  if (!LittleFS.begin()) {
    Serial.println(F("[fs] formatting LittleFS..."));
    LittleFS.format();
    LittleFS.begin();
  }

  ensureAuth();
  loadMachines();
  setupWiFi();

  if (WiFi.status() == WL_CONNECTED && MDNS_NAME[0]) {
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", WEB_PORT);
      Serial.printf("[mdns] http://%s.local\n", MDNS_NAME);
    }
  }

  setupRoutes();
  server.begin();
  Serial.printf("[web] listening on port %d\n", WEB_PORT);
}

void loop() {
  server.handleClient();
  serviceChecks();
  if (gRebootPending && (int32_t)(millis() - gRebootAt) >= 0) {
    Serial.println(F("[wifi] applying new settings, rebooting..."));
    ESP.restart();
  }
}
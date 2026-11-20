#pragma once

// Single-page web UI stored in flash (PROGMEM). Served at "/" and any
// non-/api path. The page boots by asking /api/session whether a valid
// cookie exists; if not it shows the login screen.

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex,nofollow">
<title>Wake-on-LAN Controller</title>
<style>
  :root{
    --bg:#0f1420; --card:#171e2e; --card2:#1d2638; --line:#2a3550;
    --txt:#e6ebf5; --dim:#8a97b1; --acc:#4f8cff; --ok:#2ecc71; --bad:#e74c3c;
    --warn:#f39c12; --gray:#5b6a85; --danger:#c0392b;
  }
  *{box-sizing:border-box}
  body{margin:0;font:15px/1.45 system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--txt)}
  .hidden{display:none!important}
  .mono{font-family:ui-monospace,Consolas,monospace;font-size:13px}
  .view{max-width:900px;margin:0 auto;padding:24px 16px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:20px;margin-bottom:18px}
  h1{font-size:22px;margin:0 0 14px}
  h2{font-size:16px;margin:0 0 12px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px}
  label{display:block;margin:10px 0 4px;color:var(--dim);font-size:13px}
  input{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--card2);color:var(--txt);font-size:15px}
  input:focus{outline:none;border-color:var(--acc)}
  button{padding:9px 16px;border:0;border-radius:8px;background:var(--acc);color:#fff;font-size:14px;cursor:pointer}
  button:hover{filter:brightness(1.15)}
  button.ghost{background:var(--card2);border:1px solid var(--line);color:var(--txt)}
  button.danger{background:var(--danger)}
  button.link{background:none;padding:2px 6px;color:var(--dim);text-decoration:underline}
  button.link:hover{color:var(--txt);filter:none}
  table{width:100%;border-collapse:collapse;margin-top:6px}
  th,td{text-align:left;padding:10px 8px;border-bottom:1px solid var(--line);vertical-align:middle}
  th{color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.4px}
  tr:hover td{background:rgba(255,255,255,.02)}
  .dot{display:inline-block;width:11px;height:11px;border-radius:50%;margin-right:7px;background:var(--gray);vertical-align:-1px}
  .dot.awake{background:var(--ok);box-shadow:0 0 8px var(--ok)}
  .dot.asleep{background:var(--bad)}
  .dot.waking{background:var(--warn);animation:pulse 1.2s infinite}
  .dot.none{background:var(--gray)}
  @keyframes pulse{50%{opacity:.35}}
  .status-unknown{color:var(--dim)}
  header.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px;flex-wrap:wrap;gap:10px}
  header.top h1{margin:0}
  .actions{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
  .who{color:var(--dim)}
  .msg{padding:10px 14px;border-radius:8px;margin-bottom:14px;font-size:14px}
  .msg.err{background:rgba(231,76,60,.15);border:1px solid rgba(231,76,60,.5);color:#ffb3ab}
  .msg.ok{background:rgba(46,204,113,.12);border:1px solid rgba(46,204,113,.4);color:#a9f0c6}
  .err{color:#ffb3ab;font-size:13px;min-height:18px;margin:6px 0}
  .hint{color:var(--dim);font-size:12px;margin-top:4px}
  .row{display:flex;gap:12px}
  .row>div{flex:1}
  .btnrow{margin-top:16px;display:flex;gap:10px}
  .login{max-width:360px;margin:8vh auto 0}
  .login h1{text-align:center}
  #del-msg,#save-msg,#wake-msg,.flash{position:fixed;left:50%;transform:translateX(-50%);bottom:24px;z-index:9;max-width:90%}
  code{background:var(--card2);padding:1px 5px;border-radius:4px}
  input[type=checkbox]{width:auto;transform:scale(1.25);margin-right:8px}
  label.inline{display:inline-flex;align-items:center;color:var(--txt);font-size:14px}
  @media (max-width:640px){ .hide-sm{display:none} }
</style>
</head>
<body>

<!-- ================= LOGIN ================= -->
<div id="login-view" class="view login hidden">
  <div class="card">
    <h1>&#128268; Wake-on-LAN</h1>
    <div class="msg err hidden" id="login-msg"></div>
    <form id="login-form" autocomplete="on">
      <label for="lg-user">Username</label>
      <input id="lg-user" autocomplete="username" required>
      <label for="lg-pass">Password</label>
      <input id="lg-pass" type="password" autocomplete="current-password" required>
      <div class="err" id="lg-err"></div>
      <button style="width:100%" type="submit">Sign in</button>
    </form>
  </div>
</div>

<!-- ================= DASHBOARD ================= -->
<div id="app-view" class="view hidden">
  <header class="top">
    <h1>&#128268; Wake-on-LAN Controller</h1>
    <div class="actions">
      <button id="btn-add">+ Add machine</button>
      <span class="who" id="who"></span>
      <button class="ghost" id="btn-logout">Log out</button>
    </div>
  </header>

  <div class="msg hidden" id="banner"></div>

  <div class="card">
    <table>
      <thead><tr>
        <th>Machine</th><th>Status</th><th class="hide-sm">MAC</th>
        <th class="hide-sm">IP</th><th>Last check</th><th></th>
      </tr></thead>
      <tbody id="rows"></tbody>
    </table>
    <div class="hint" id="empty-hint" style="display:none">
      No machines configured yet. Click <b>+ Add machine</b> to register one &mdash;
      need its MAC address (shown on the motherboard / in the OS, e.g.
      <code>AA-BB-CC-DD-EE-FF</code>).
    </div>
    <div class="hint" id="probe-hint" style="display:none">
      Status dots: <span class="dot awake"></span>awake &nbsp;
      <span class="dot asleep"></span>asleep &nbsp;
      <span class="dot waking"></span>waking up &nbsp;
      <span class="dot none"></span>no check port configured
    </div>
  </div>

  <!-- Add / edit machine -->
  <div class="card hidden" id="editor">
    <h2 id="ed-title">Add machine</h2>
    <div class="err" id="ed-err"></div>
    <form id="ed-form">
      <label>Name</label>
      <input id="ed-name" maxlength="32" required placeholder="Living-room PC">
      <div class="row">
        <div>
          <label>MAC address</label>
          <input id="ed-mac" class="mono" required placeholder="AA:BB:CC:DD:EE:FF">
        </div>
        <div>
          <label>IP address (optional, improves delivery)</label>
          <input id="ed-ip" class="mono" placeholder="192.168.1.50">
        </div>
      </div>
      <div class="row">
        <div>
          <label>WOL UDP port</label>
          <input id="ed-wolport" type="number" min="1" max="65535" value="9">
        </div>
        <div>
          <label>Check port (0 = never probe)</label>
          <input id="ed-checkport" type="number" min="0" max="65535" value="3389">
          <div class="hint">Use a port the machine answers when on, e.g. 3389 (RDP), 445 (SMB), 22 (SSH)</div>
        </div>
      </div>
      <div class="btnrow">
        <button type="submit">Save</button>
        <button type="button" class="ghost" id="ed-cancel">Cancel</button>
      </div>
    </form>
  </div>

  <!-- Change password -->
  <div class="card" id="pw-card">
    <h2>Change credentials</h2>
    <div class="err" id="pw-err"></div>
    <form id="pw-form">
      <div class="row">
        <div>
          <label>Current password</label>
          <input id="pw-cur" type="password" autocomplete="current-password" required>
        </div>
        <div>
          <label>Username (optional)</label>
          <input id="pw-user" autocomplete="username" placeholder="keep current">
        </div>
      </div>
      <div class="row">
        <div>
          <label>New password</label>
          <input id="pw-new" type="password" autocomplete="new-password" required minlength="8" maxlength="64">
        </div>
        <div>
          <label>Repeat new password</label>
          <input id="pw-new2" type="password" autocomplete="new-password" required>
        </div>
      </div>
      <div class="btnrow"><button type="submit">Update credentials</button></div>
    </form>
  </div>

  <!-- WiFi settings -->
  <div class="card" id="wifi-card">
    <h2>WiFi</h2>
    <div class="hint" id="wifi-status">Loading&hellip;</div>
    <div class="err" id="wf-err"></div>
    <form id="wf-form">
      <div class="row">
        <div>
          <label>SSID</label>
          <input id="wf-ssid" maxlength="32" required placeholder="MyNetwork">
        </div>
        <div>
          <label>Password (leave empty = keep current)</label>
          <input id="wf-pass" type="password" autocomplete="new-password" maxlength="64">
        </div>
      </div>
      <label class="inline"><input id="wf-dhcp" type="checkbox" checked> Use DHCP (automatic IP)</label>
      <div id="wf-static" class="hidden">
        <div class="row">
          <div>
            <label>Static IP</label>
            <input id="wf-ip" class="mono" placeholder="192.168.1.42">
          </div>
          <div>
            <label>Netmask</label>
            <input id="wf-nm" class="mono" placeholder="255.255.255.0">
          </div>
        </div>
        <div class="row">
          <div>
            <label>Gateway</label>
            <input id="wf-gw" class="mono" placeholder="192.168.1.1">
          </div>
          <div>
            <label>DNS</label>
            <input id="wf-dns" class="mono" placeholder="192.168.1.1">
          </div>
        </div>
      </div>
      <div class="btnrow"><button type="submit">Apply &amp; reboot</button></div>
    </form>
  </div>
</div>

<script>
"use strict";
const $ = s => document.querySelector(s);
let CSRF = "";
let EDIT_ID = null;
let AUTO_T = null;

const esc = s => String(s ?? "").replace(/[&<>"']/g,
  c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));

async function req(url, body, method) {
  const opts = { method: method || (body === undefined ? "GET" : "POST"), headers: {} };
  if (body !== undefined) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  if (CSRF) opts.headers["X-CSRF-Token"] = CSRF;
  const r = await fetch(url, opts);
  let data = {};
  try { data = await r.json(); } catch (e) {}
  if (r.status === 401 && url !== "/api/login") { showLogin(); throw new Error("logged out"); }
  if (!r.ok) throw new Error(data.error || ("HTTP " + r.status));
  return data;
}

function showLogin() {
  clearInterval(AUTO_T); AUTO_T = null; CSRF = "";
  $("#app-view").classList.add("hidden");
  $("#login-view").classList.remove("hidden");
  $("#lg-user").focus();
}

function showApp() {
  $("#login-view").classList.add("hidden");
  $("#app-view").classList.remove("hidden");
}

function banner(text, ok) {
  const b = $("#banner");
  if (!text) { b.classList.add("hidden"); return; }
  b.textContent = text;
  b.className = "msg " + (ok ? "ok" : "err");
  setTimeout(() => b.classList.add("hidden"), ok ? 2500 : 6000);
}

function dotClass(m) {
  if (m.status === "awake") return "awake";
  if (m.status === "asleep") return "asleep";
  if (m.status === "waking") return "waking";
  return "none";
}

function statusText(m) {
  if (m.status === "awake") return "Awake";
  if (m.status === "asleep") return "Asleep";
  if (m.status === "waking") return "Waking&hellip;";
  return "No check port";
}

function lastCheckText(m) {
  if (!m.checked) return "&mdash;";
  const s = m.lastCheckAgo;
  if (s < 5) return "just now";
  if (s < 90) return s + " s ago";
  return Math.round(s / 60) + " min ago";
}

function render(list) {
  const tb = $("#rows");
  tb.innerHTML = "";
  $("#empty-hint").style.display = list.length ? "none" : "block";
  $("#probe-hint").style.display = list.length ? "block" : "none";
  for (const m of list) {
    const tr = document.createElement("tr");
    tr.innerHTML =
      `<td><b>${esc(m.name)}</b></td>` +
      `<td><span class="dot ${dotClass(m)}"></span>${statusText(m)}</td>` +
      `<td class="mono hide-sm">${esc(m.mac)}</td>` +
      `<td class="mono hide-sm">${esc(m.ip || "")}</td>` +
      `<td>${lastCheckText(m)}</td>` +
      `<td style="white-space:nowrap;text-align:right">
         <button class="wake" data-id="${esc(m.id)}">Wake</button>
         <button class="link edit" data-id="${esc(m.id)}">edit</button>
         <button class="link del" data-id="${esc(m.id)}">delete</button>
       </td>`;
    tb.appendChild(tr);
  }
}

async function refresh(quiet) {
  try {
    const data = await req("/api/machines");
    render(data.machines);
  } catch (e) {
    if (!quiet) banner(e.message, false);
  }
}

async function boot() {
  let s;
  try {
    s = await fetch("/api/session").then(r => r.json());
  } catch (e) { $("#lg-err").textContent = "Cannot reach device."; return; }
  if (!s.authenticated) { showLogin(); return; }
  CSRF = s.csrf;
  $("#who").textContent = s.user;
  showApp();
  await refresh(true);
  await loadWifiStatus();
  clearInterval(AUTO_T);
  AUTO_T = setInterval(() => refresh(true), 10000);
}

/* ---------- login / logout ---------- */
$("#login-form").addEventListener("submit", async e => {
  e.preventDefault();
  $("#lg-err").textContent = "";
  const user = $("#lg-user").value.trim(), password = $("#lg-pass").value;
  try {
    const d = await req("/api/login", { user, password });
    CSRF = d.csrf;
    $("#who").textContent = d.user;
    $("#lg-pass").value = "";
    showApp();
    await refresh(true);
    await loadWifiStatus();
    clearInterval(AUTO_T);
    AUTO_T = setInterval(() => refresh(true), 10000);
  } catch (err) {
    $("#lg-err").textContent = err.message;
  }
});

$("#btn-logout").addEventListener("click", async () => {
  try { await req("/api/logout", {}); } catch (e) {}
  showLogin();
});

/* ---------- list actions ---------- */
$("#rows").addEventListener("click", async e => {
  const id = e.target.dataset && e.target.dataset.id;
  if (!id) return;
  if (e.target.classList.contains("wake")) {
    e.target.disabled = true;
    try {
      const d = await req("/api/wake", { id });
      banner("Magic packet sent to " + d.name + " \u2014 it should wake within a minute.", true);
      await refresh(true);
    } catch (err) { banner(err.message, false); }
    setTimeout(() => { e.target.disabled = false; }, 3000);
  } else if (e.target.classList.contains("edit")) {
    try {
      const d = await req("/api/machines");
      const m = d.machines.find(x => x.id === id);
      if (m) openEditor(m);
    } catch (err) { banner(err.message, false); }
  } else if (e.target.classList.contains("del")) {
    if (!confirm("Delete this machine?")) return;
    try {
      await req("/api/machines/delete", { id });
      await refresh(true);
    } catch (err) { banner(err.message, false); }
  }
});

/* ---------- add / edit ---------- */
function openEditor(m) {
  EDIT_ID = m ? m.id : null;
  $("#ed-title").textContent = m ? "Edit machine" : "Add machine";
  $("#ed-name").value = m ? m.name : "";
  $("#ed-mac").value = m ? m.mac : "";
  $("#ed-ip").value = m ? (m.ip || "") : "";
  $("#ed-wolport").value = m ? m.wolPort : 9;
  $("#ed-checkport").value = m ? m.checkPort : 3389;
  $("#ed-err").textContent = "";
  $("#editor").classList.remove("hidden");
  $("#editor").scrollIntoView({ behavior: "smooth" });
  $("#ed-name").focus();
}

$("#btn-add").addEventListener("click", () => openEditor(null));
$("#ed-cancel").addEventListener("click", () => $("#editor").classList.add("hidden"));

$("#ed-form").addEventListener("submit", async e => {
  e.preventDefault();
  $("#ed-err").textContent = "";
  const body = {
    name: $("#ed-name").value.trim(),
    mac: $("#ed-mac").value.trim(),
    ip: $("#ed-ip").value.trim(),
    wolPort: parseInt($("#ed-wolport").value, 10) || 9,
    checkPort: parseInt($("#ed-checkport").value, 10) || 0
  };
  if (EDIT_ID) body.id = EDIT_ID;
  try {
    await req("/api/machines", body);
    $("#editor").classList.add("hidden");
    banner(EDIT_ID ? "Machine updated." : "Machine added.", true);
    EDIT_ID = null;
    await refresh(true);
  } catch (err) {
    $("#ed-err").textContent = err.message;
  }
});

/* ---------- change credentials ---------- */
$("#pw-form").addEventListener("submit", async e => {
  e.preventDefault();
  $("#pw-err").textContent = "";
  if ($("#pw-new").value !== $("#pw-new2").value) {
    $("#pw-err").textContent = "New passwords do not match.";
    return;
  }
  try {
    const d = await req("/api/password", {
      current: $("#pw-cur").value,
      user: $("#pw-user").value.trim(),
      password: $("#pw-new").value
    });
    $("#who").textContent = d.user || $("#who").textContent;
    $("#pw-cur").value = $("#pw-new").value = $("#pw-new2").value = $("#pw-user").value = "";
    banner("Credentials updated.", true);
  } catch (err) {
    $("#pw-err").textContent = err.message;
  }
});

/* ---------- wifi configuration ---------- */
function toggleStatic() {
  document.querySelectorAll("#wf-static").forEach(el => {
    el.style.display = $("#wf-dhcp").checked ? "none" : "block";
  });
}
$("#wf-dhcp").addEventListener("change", toggleStatic);

async function loadWifiStatus() {
  let w;
  try { w = await req("/api/wifi"); } catch (e) { return; }
  const st = $("#wifi-status");
  if (w.mode === "ap") {
    st.innerHTML = "&#9888; Setup AP mode &mdash; connect to the device hotspot, then open <b>http://" +
                   esc(w.address) + "</b> and fix the settings below.";
  } else if (w.connected) {
    st.textContent = "Connected to \"" + w.ssid + "\" \u2014 IP " + w.address;
  } else {
    st.textContent = "Not connected (SSID \"" + w.ssid + "\")";
  }
  $("#wf-ssid").value = w.ssid || "";
  $("#wf-pass").value = "";
  $("#wf-dhcp").checked = !!w.dhcp;
  $("#wf-ip").value = w.ip || "";
  $("#wf-nm").value = w.netmask || "";
  $("#wf-gw").value = w.gateway || "";
  $("#wf-dns").value = w.dns || "";
  toggleStatic();
}

$("#wf-form").addEventListener("submit", async e => {
  e.preventDefault();
  $("#wf-err").textContent = "";
  const body = {
    ssid: $("#wf-ssid").value.trim(),
    password: $("#wf-pass").value,
    dhcp: $("#wf-dhcp").checked,
    ip: $("#wf-ip").value.trim(),
    netmask: $("#wf-nm").value.trim(),
    gateway: $("#wf-gw").value.trim(),
    dns: $("#wf-dns").value.trim()
  };
  try {
    await req("/api/wifi", body);
    clearInterval(AUTO_T); AUTO_T = null;
    banner("Saved \u2014 the device is rebooting. If it cannot reach that network, " +
           "it will broadcast the setup hotspot again.", true);
  } catch (err) {
    $("#wf-err").textContent = err.message;
  }
});

/* ---------- misc helpers ---------- */
document.addEventListener("keydown", e => {
  if (e.key === "Escape") $("#editor").classList.add("hidden");
});

boot();
</script>
</body>
</html>)rawliteral";
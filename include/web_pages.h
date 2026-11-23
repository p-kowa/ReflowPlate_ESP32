#pragma once
#include <Arduino.h>
#include "language.h"

static const char OTA_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Firmware-Update</title>
<h1>Firmware-Update</h1>
<form id="update"><label>Firmware <input id="firmware" type="file" accept=".bin" required></label>
<button type="submit">Aktualisieren</button></form>
<p id="status" role="status"></p>
<script>
const form = document.getElementById('update');
const statusText = document.getElementById('status');
form.addEventListener('submit', async (event) => {
  event.preventDefault();
  const file = document.getElementById('firmware').files[0];
  const button = form.querySelector('button');
  button.disabled = true;
  statusText.textContent = 'Update wird geschrieben ...';
  try {
    const response = await fetch('/update', {method: 'POST', credentials: 'same-origin',
      headers: {'Content-Type': 'application/octet-stream', 'X-Firmware-Upload': '1'}, body: file});
    statusText.textContent = await response.text();
    if (response.ok) return;
  } catch (error) {
    statusText.textContent = 'Verbindung unterbrochen. Bitte Geraet und Firmwarestand pruefen.';
  }
  button.disabled = false;
});
</script></html>)HTML";

// ==========================================
// 1. Reflow Plate(main page)
// ==========================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang=")rawliteral" T_HTML_LANG R"rawliteral(">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Reflow Plate</title>
  <link rel="stylesheet" href="/ui.css">
</head>
<body>
  <main class="container">
    <section class="card">
      <h1>Reflow Plate</h1>
      <dl class="readings">
        <div><dt>)rawliteral" T_PLATE_ACTUAL R"rawliteral(</dt><dd id="actual">--</dd></div>
        <div><dt>)rawliteral" T_PLATE_TARGET R"rawliteral(</dt><dd id="target">--</dd></div>
        <div><dt>)rawliteral" T_PLATE_ELAPSED R"rawliteral(</dt><dd id="elapsed">--:--:--</dd></div>
        <div><dt><label for="limit">)rawliteral" T_PLATE_LIMIT R"rawliteral(</label></dt><dd>
          <select id="limit" disabled>
            <option value="15">15 min</option>
            <option value="30">30 min</option>
            <option value="45">45 min</option>
            <option value="0" disabled>)rawliteral" T_PLATE_LIMIT_UNLIMITED R"rawliteral(</option>
          </select></dd></div>
      </dl>
      <div class="row stepper">
        <button id="minus" class="btn-secondary" aria-label=")rawliteral" T_PLATE_DECREASE R"rawliteral(" disabled>&minus;</button>
        <output id="draft" aria-label=")rawliteral" T_PLATE_DRAFT R"rawliteral(">--</output>
        <button id="plus" class="btn-secondary" aria-label=")rawliteral" T_PLATE_INCREASE R"rawliteral(" disabled>+</button>
      </div>
      <div class="row">
        <button id="apply" class="btn-primary" disabled>)rawliteral" T_PLATE_SET R"rawliteral(</button>
        <button id="stop" class="btn-secondary" disabled>)rawliteral" T_PLATE_STOP R"rawliteral(</button>
      </div>
      <div id="plateError" class="status-msg error" role="status">)rawliteral" T_PLATE_REQUEST_FAILED R"rawliteral(</div>
      <label><input type="checkbox" id="otaOpen" disabled> )rawliteral" T_PLATE_OTA_NO_PASSWORD R"rawliteral(</label>
      <a href="/setup" class="settings-link">)rawliteral" T_PLATE_SETTINGS R"rawliteral(</a>
    </section>
  </main>
  <script>
    const ui = Object.fromEntries([...document.querySelectorAll('[id]')].map(el => [el.id, el]));
    const temperature = new Intl.NumberFormat(document.documentElement.lang, {maximumFractionDigits: 1});
    const degrees = value => temperature.format(value) + ' \u00b0C';
    let state, draft, edited = false, busy = false, polling = false, revision = 0;

    function render() {
      ui.actual.textContent = state.actual === null ? '--' : degrees(state.actual);
      ui.target.textContent = degrees(state.target);
      ui.draft.textContent = degrees(draft);
      ui.elapsed.textContent = [Math.floor(state.elapsed / 3600), Math.floor(state.elapsed / 60) % 60, state.elapsed % 60]
        .map(value => String(value).padStart(2, '0')).join(':');
      ui.minus.disabled = busy || draft <= state.min;
      ui.plus.disabled = busy || state.min > state.max || draft >= state.max;
      ui.apply.disabled = busy || !state.canSet || draft < state.min;
      ui.stop.disabled = busy || !state.running;
      ui.otaOpen.disabled = busy;
      if (!busy) ui.otaOpen.checked = state.otaOpen;
      ui.limit.disabled = busy;
      if (!busy && document.activeElement !== ui.limit) ui.limit.value = String(state.limit);
    }

    function adjust(direction) {
      draft = Math.max(state.min, draft + direction * state.step);
      edited = true;
      render();
    }

    async function request(path, options) {
      if (busy || (!options && polling)) return;
      if (options) {
        busy = true;
        revision += 1;
        render();
      } else {
        polling = true;
      }
      const requestRevision = revision;
      try {
        const response = await fetch(path, options);
        if (!response.ok) throw new Error(response.status);
        const incoming = await response.json();
        if (requestRevision !== revision) return;
        state = incoming;
        if (path === '/api/plate' && options) edited = false;
        if (!edited) draft = state.target;
        ui.plateError.style.display = 'none';
      } catch {
        if (requestRevision === revision) ui.plateError.style.display = 'block';
      } finally {
        if (options) busy = false;
        else polling = false;
        if (state && requestRevision === revision) render();
      }
    }

    ui.minus.onclick = () => adjust(-1);
    ui.plus.onclick = () => adjust(1);
    ui.apply.onclick = () => request('/api/plate', {method: 'POST',
      headers: {'X-Reflow-Control': '1'}, body: new URLSearchParams({target: draft})});
    ui.stop.onclick = () => request('/api/plate/stop', {method: 'POST'});
    ui.limit.onchange = () => request('/api/plate/limit', {method: 'POST',
      headers: {'X-Reflow-Control': '1'}, body: new URLSearchParams({minutes: ui.limit.value})});
    ui.otaOpen.onchange = async () => {
      const open = ui.otaOpen.checked;
      if (open && !confirm(')rawliteral" T_PLATE_OTA_CONFIRM R"rawliteral(')) {
        ui.otaOpen.checked = false;
        return;
      }
      busy = true;
      revision += 1;
      render();
      try {
        const response = await fetch('/api/ota', {method: 'POST', headers: {'X-Reflow-Control': '1'},
          body: new URLSearchParams({open: open ? '1' : '0'})});
        if (!response.ok) throw new Error(response.status);
        state.otaOpen = open;
        ui.plateError.style.display = 'none';
      } catch {
        ui.plateError.style.display = 'block';
      } finally {
        busy = false;
        render();
      }
    };
    request('/api/plate');
    setInterval(() => request('/api/plate'), 1000);
  </script>
</body>
</html>
)rawliteral";


const char UI_CSS[] PROGMEM = R"rawliteral(
    :root {
      --bg: #0f172a;
      --card-bg: #1e293b;
      --accent: #38bdf8;
      --accent-hover: #0284c7;
      --text: #f8fafc;
      --text-muted: #94a3b8;
      --border: #334155;
      --active: #10b981;
      --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background-color: var(--bg); color: var(--text); padding: 1.5rem; display: flex; justify-content: center; min-height: 100vh; }
    .container { max-width: 480px; width: 100%; display: flex; flex-direction: column; gap: 1.25rem; }
    .card { background: var(--card-bg); border-radius: 1rem; padding: 1.5rem; border: 1px solid var(--border); box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); }
    h1 { font-size: 1.4rem; color: var(--accent); margin-bottom: 0.25rem; text-align: center; }
    .subtitle { text-align: center; font-size: 0.85rem; color: var(--text-muted); margin-bottom: 1.25rem; }
    
    .form-group { display: flex; flex-direction: column; gap: 0.4rem; margin-bottom: 1rem; }
    label { font-size: 0.85rem; color: var(--text-muted); font-weight: 600; }
    select, input[type=text], input[type=password], input[type=url] { background: #0f172a; border: 1px solid var(--border); border-radius: 0.5rem; color: var(--text); padding: 0.75rem; font-size: 0.95rem; outline: none; width: 100%; }
    select:focus, input:focus { border-color: var(--accent); }
    
    .row { display: flex; gap: 0.5rem; }
    button { cursor: pointer; border: none; border-radius: 0.5rem; font-weight: 600; padding: 0.75rem 1rem; transition: all 0.2s; font-size: 0.95rem; width: 100%; }
    .btn-primary { background: var(--accent); color: #0f172a; }
    .btn-primary:hover { background: var(--accent-hover); }
    .btn-secondary { background: #334155; color: var(--text); }
    .btn-secondary:hover { background: #475569; }
    .btn-danger { background: var(--danger); color: white; margin-top: 1rem; }
    .btn-danger:hover { opacity: 0.9; }
    
    .status-msg { margin-top: 1rem; padding: 0.75rem; border-radius: 0.5rem; font-size: 0.85rem; text-align: center; display: none; }
    .status-msg.info { background: rgba(56, 189, 248, 0.15); color: var(--accent); border: 1px solid var(--accent); }
    .status-msg.success { background: rgba(16, 185, 129, 0.15); color: #34d399; border: 1px solid var(--active); }
    .status-msg.error { background: rgba(239, 68, 68, 0.15); color: #f87171; border: 1px solid var(--danger); }
    
    .spinner { display: inline-block; width: 16px; height: 16px; border: 2px solid rgba(255,255,255,.3); border-radius: 50%; border-top-color: #fff; animation: spin 1s ease-in-out infinite; vertical-align: middle; margin-right: 0.4rem; }
    @keyframes spin { to { transform: rotate(360deg); } }
    
    .nav-link { display: block; text-align: center; margin-top: 1rem; color: var(--text-muted); text-decoration: none; font-size: 1.6rem; }
    .nav-link:hover { color: var(--accent); }
    .readings { display: grid; gap: 0.75rem; margin-bottom: 1.25rem; }
    .readings div { display: flex; flex-wrap: wrap; align-items: baseline; justify-content: space-between; gap: 0.35rem 0.75rem; border-bottom: 1px solid var(--border); padding-bottom: 0.5rem; }
    .readings dt { color: var(--text-muted); }
    .readings dd, .stepper output { font-weight: 600; font-variant-numeric: tabular-nums; white-space: nowrap; }
    .stepper { display: grid; grid-template-columns: 3rem minmax(0, 1fr) 3rem; align-items: center; margin-bottom: 0.75rem; }
    .stepper button { padding: 0; height: 2.75rem; font-size: 1.35rem; }
    .stepper output { text-align: center; font-size: 1.2rem; }
    button:disabled { opacity: 0.5; cursor: default; }
    .settings-link { display: block; text-align: center; margin-top: 1rem; color: var(--accent); }
)rawliteral";

// ==========================================
// 2. Wi-Fi setup web interface (configuration)
// ==========================================
const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang=")rawliteral" T_HTML_LANG R"rawliteral(">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>)rawliteral" T_PLATE_SETTINGS R"rawliteral( | Reflow Plate</title>
  <link rel="stylesheet" href="/ui.css">
</head>
<body>
  <div class="container">
    <div class="card">
      <h1>)rawliteral" T_SETUP_HEADING R"rawliteral(</h1>
      <div class="subtitle" id="wifiSubtitle" aria-live="polite">Reflow Plate</div>

      <div class="form-group">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.3rem;">
          <label for="ssidSelect">)rawliteral" T_SETUP_NETWORKS_LABEL R"rawliteral(</label>
          <button type="button" id="scanButton" class="btn-secondary" onclick="scanWifi()" style="width: auto; padding: 0.3rem 0.6rem; font-size: 0.75rem;">)rawliteral" T_SETUP_SCAN_BUTTON R"rawliteral(</button>
        </div>
        <select id="ssidSelect" onchange="onSsidSelect()">
          <option value="">)rawliteral" T_SETUP_SCAN_HINT R"rawliteral(</option>
        </select>
      </div>

      <div class="form-group">
        <label for="manualSsid">)rawliteral" T_SETUP_MANUAL_SSID_LABEL R"rawliteral(</label>
        <input type="text" id="manualSsid" placeholder=")rawliteral" T_SETUP_SSID_PLACEHOLDER R"rawliteral(">
      </div>

      <div class="form-group">
        <label for="wifiPass">)rawliteral" T_SETUP_WIFI_PASS_LABEL R"rawliteral(</label>
        <input type="password" id="wifiPass" placeholder=")rawliteral" T_SETUP_WIFI_PASS_PLACEHOLDER R"rawliteral(">
      </div>

      <button class="btn-primary" onclick="saveWifi()">)rawliteral" T_SETUP_SAVE_CONNECT R"rawliteral(</button>

      <div id="statusMsg" class="status-msg" role="status"></div>

      <a href="/" class="settings-link">&larr; Reflow Plate</a>
    </div>

    <div class="card" style="margin-top: 0.5rem;">
      <h3 style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 0.5rem;">)rawliteral" T_SETUP_OPTIONS_HEADING R"rawliteral(</h3>
      <button class="btn-danger" onclick="resetWifi()">)rawliteral" T_SETUP_RESET_BUTTON R"rawliteral(</button>
    </div>

    <div class="card">
      <h3 style="font-size: 0.9rem; color: var(--text-muted); margin-bottom: 0.75rem;">MQTT / Home Assistant</h3>
      <div class="form-group">
        <label for="mqttHost">)rawliteral" T_SETUP_MQTT_HOST_LABEL R"rawliteral(</label>
        <input type="text" id="mqttHost">
      </div>
      <div class="form-group" id="groupMqttPort">
        <label for="mqttPort">)rawliteral" T_SETUP_PORT_LABEL R"rawliteral(</label>
        <input type="text" id="mqttPort" value="1883" inputmode="numeric">
      </div>
      <div class="form-group" id="groupMqttCreds">
        <label for="mqttUser">)rawliteral" T_SETUP_MQTT_USER_LABEL R"rawliteral(</label>
        <input type="text" id="mqttUser" placeholder=")rawliteral" T_SETUP_MQTT_LEAVE_EMPTY R"rawliteral(">
      </div>
      <div class="form-group" id="groupMqttPass">
        <label for="mqttPass">)rawliteral" T_SETUP_MQTT_PASS_LABEL R"rawliteral(</label>
        <input type="password" id="mqttPass" placeholder=")rawliteral" T_PLATE_KEEP_MQTT_PASS R"rawliteral(" autocomplete="new-password">
        <label><input type="checkbox" id="clearMqttPass"> )rawliteral" T_PLATE_CLEAR_MQTT_PASS R"rawliteral(</label>
      </div>
      <div class="form-group" id="groupMqttBase">
        <label for="mqttBase">)rawliteral" T_SETUP_MQTT_BASE_LABEL R"rawliteral(</label>
        <input type="text" id="mqttBase" value="reflowplate">
      </div>
      <button id="mqttSave" class="btn-primary" onclick="saveMqtt()" disabled>)rawliteral" T_SETUP_MQTT_SAVE R"rawliteral(</button>
      <div id="mqttMsg" class="status-msg" role="status"></div>
    </div>
  </div>

  <script>
    function showMsg(text, type, id = 'statusMsg') {
      const msg = document.getElementById(id);
      msg.className = 'status-msg ' + type;
      msg.textContent = text;
      msg.style.display = 'block';
    }

    async function scanWifi() {
      const button = document.getElementById('scanButton');
      if (button.disabled) return;
      button.disabled = true;
      const sel = document.getElementById('ssidSelect');
      sel.replaceChildren(new Option(')rawliteral" T_SETUP_SEARCHING_NETWORKS R"rawliteral(', ''));
      showMsg(')rawliteral" T_SETUP_SCANNING R"rawliteral(', 'info');
      try {
        for (let attempt = 0; attempt < 30; attempt += 1) {
          const response = await fetch('/api/wifi/scan');
          if (response.status === 202) {
            await new Promise(resolve => setTimeout(resolve, 750));
            continue;
          }
          if (!response.ok) throw new Error(response.status);
          const networks = await response.json();
          sel.replaceChildren(new Option(')rawliteral" T_SETUP_SELECT_NETWORK R"rawliteral(', ''));
          networks.forEach(net => sel.add(new Option(`${net.ssid} (${net.rssi} dBm)`, net.ssid)));
          showMsg(networks.length ? `)rawliteral" T_SETUP_NETWORKS_FOUND_PREFIX R"rawliteral(${networks.length})rawliteral" T_SETUP_NETWORKS_FOUND_SUFFIX R"rawliteral(` : ')rawliteral" T_SETUP_NO_NETWORKS R"rawliteral(', 'info');
          return;
        }
        throw new Error('timeout');
      } catch {
        sel.replaceChildren(new Option(')rawliteral" T_SETUP_SCAN_HINT R"rawliteral(', ''));
        showMsg(')rawliteral" T_SETUP_SCAN_ERROR R"rawliteral(', 'error');
      } finally {
        button.disabled = false;
      }
    }

    function onSsidSelect() {
      const val = document.getElementById('ssidSelect').value;
      if (val) {
        document.getElementById('manualSsid').value = val;
      }
    }

    async function saveWifi() {
      const ssid = document.getElementById('manualSsid').value;
      const pass = document.getElementById('wifiPass').value;

      if (!ssid) {
        showMsg(')rawliteral" T_SETUP_SSID_REQUIRED R"rawliteral(', 'error');
        return;
      }

      showMsg(')rawliteral" T_SETUP_SAVING_PREFIX R"rawliteral(' + ssid + ')rawliteral" T_SETUP_SAVING_SUFFIX R"rawliteral(', 'info');
      try {
        await post('/api/wifi/save', {ssid, pass});
        showMsg(')rawliteral" T_SETUP_SAVED R"rawliteral( )rawliteral" T_SETUP_SAVED_HOTSPOT_HINT R"rawliteral(', 'success');
      } catch {
        showMsg(')rawliteral" T_SETUP_SAVE_ERROR R"rawliteral(', 'error');
      }
    }

    async function resetWifi() {
      if (!confirm(')rawliteral" T_SETUP_RESET_CONFIRM R"rawliteral(')) return;
      try {
        await post('/api/wifi/reset');
        showMsg(')rawliteral" T_SETUP_RESET_DONE R"rawliteral(', 'info');
      } catch {
        showMsg(')rawliteral" T_SETUP_SAVE_ERROR R"rawliteral(', 'error');
      }
    }

    async function post(url, fields) {
      const response = await fetch(url, {method: 'POST', body: new URLSearchParams(fields)});
      if (!response.ok) throw new Error(response.status);
    }

    async function loadMqtt() {
      try {
        const response = await fetch('/api/mqtt/config');
        if (!response.ok) throw new Error(response.status);
        const data = await response.json();
        document.getElementById('mqttHost').value = data.host;
        document.getElementById('mqttPort').value = data.port;
        document.getElementById('mqttUser').value = data.user;
        document.getElementById('mqttBase').value = data.base;
        document.getElementById('mqttSave').disabled = false;
      } catch {
        showMsg(')rawliteral" T_PLATE_REQUEST_FAILED R"rawliteral(', 'error', 'mqttMsg');
      }
    }

    async function saveMqtt() {
      const button = document.getElementById('mqttSave');
      const password = document.getElementById('mqttPass');
      const clearPassword = document.getElementById('clearMqttPass');
      button.disabled = true;
      try {
        await post('/api/mqtt/config', {
          host: document.getElementById('mqttHost').value.trim(),
          port: document.getElementById('mqttPort').value.trim() || '1883',
          user: document.getElementById('mqttUser').value,
          pass: password.value,
          clearPass: clearPassword.checked ? '1' : '0',
          base: document.getElementById('mqttBase').value.trim() || 'reflowplate'
        });
        password.value = '';
        clearPassword.checked = false;
        showMsg(')rawliteral" T_PLATE_MQTT_SAVED R"rawliteral(', 'success', 'mqttMsg');
      } catch {
        showMsg(')rawliteral" T_SETUP_SAVE_ERROR R"rawliteral(', 'error', 'mqttMsg');
      } finally {
        button.disabled = false;
      }
    }

    // A scan briefly interrupts the Wi-Fi connection, so scan automatically only in hotspot mode.
    // While connected, the subtitle shows the current network instead.
    fetch('/api/status')
      .then(res => res.json())
      .then(data => {
        if (data.ssid) {
          document.getElementById('wifiSubtitle').innerText = ')rawliteral" T_SETUP_CONNECTED_PREFIX R"rawliteral(' + '"' + data.ssid + '"' + ')rawliteral" T_SETUP_CONNECTED_SUFFIX R"rawliteral(';
        } else if (data.ap_mode) {
          scanWifi();
        }
      })
      .catch(() => {});
    loadMqtt();
  </script>
</body>
</html>
)rawliteral";
#include "WebServerManager.h"
#include "Config.h"
#include <WiFi.h>

const char HTML_CONTENT[] = R"raw(
<!DOCTYPE html>
<html>
<head>
<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>
<title>Desky Controller</title>
<style>
* { touch-action: none; -webkit-touch-callout: none; -webkit-user-select: none; user-select: none; box-sizing: border-box; }
html, body { 
  width: 100%; height: 100%; margin: 0; padding: 0;
  background: #121212; color: #e0e0e0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  display: flex; flex-direction: column; justify-content: space-between; align-items: center; overflow: hidden;
}

/* Header & Telemetry Bar */
.header-bar {
  width: 100%; background: #1a1a1a; padding: 12px 16px; border-bottom: 1px solid #2a2a2a;
  display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 10px rgba(0,0,0,0.5);
  z-index: 10;
}
.brand { font-size: 15px; font-weight: 700; letter-spacing: 1px; color: #888; text-transform: uppercase; }
.telemetry-group { display: flex; align-items: center; gap: 16px; flex: 1; justify-content: flex-end; }
.telemetry-item { display: flex; align-items: center; gap: 6px; font-size: 13px; color: #aaa; }
.value { font-weight: 600; color: #ffffff; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: #444; transition: background 0.2s, box-shadow 0.2s; flex-shrink: 0; }
.dot-ok { background: #00e676; box-shadow: 0 0 6px #00e676; }
.dot-warn { background: #ff9100; box-shadow: 0 0 6px #ff9100; }
.dot-danger { background: #ff1744; box-shadow: 0 0 6px #ff1744; }

.icon-btn { 
  background: none; border: none; color: #888; cursor: pointer; padding: 4px; 
  display: flex; align-items: center; justify-content: center; transition: color 0.2s; 
}
.icon-btn:active { color: #fff; }
.icon-btn svg { width: 18px; height: 18px; fill: currentColor; }

/* Collapsible Settings Drawer */
.drawer {
  width: 100%; background: #181818; border-bottom: 1px solid #282828; max-height: 0; overflow: hidden;
  transition: max-height 0.3s ease-out, padding 0.3s ease; padding: 0 16px; display: flex; flex-direction: column; gap: 10px; align-items: center;
  z-index: 9;
}
.drawer.open { max-height: 140px; padding: 12px 16px; }
.setting-row { display: flex; align-items: center; gap: 12px; width: 100%; max-width: 360px; font-size: 13px; color: #aaa; }
.setting-row input { flex: 1; accent-color: #00adb5; }

/* Main Stage */
.main-stage { flex: 1; width: 100%; display: flex; flex-direction: column; justify-content: space-evenly; align-items: center; }

/* Joystick Canvas */
#joystick-container { display: flex; justify-content: center; align-items: center; width: 220px; height: 220px; }
canvas { display: block; }

/* Automotive E-Brake Button */
.ebrake-container { display: flex; flex-direction: column; align-items: center; gap: 6px; margin-bottom: 10px; }
.ebrake-btn {
  width: 64px; height: 64px; border-radius: 50%; background: #1e1e1e; border: 2px solid #444;
  display: flex; justify-content: center; align-items: center; cursor: pointer; transition: all 0.2s ease;
  box-shadow: 0 4px 12px rgba(0,0,0,0.4);
}
.ebrake-icon {
  font-size: 18px; font-weight: 900; color: #666; font-family: monospace; letter-spacing: -1px;
  border: 2px solid #666; border-radius: 50%; width: 32px; height: 32px; display: flex; align-items: center; justify-content: center;
  transition: all 0.2s ease;
}
.ebrake-label { font-size: 10px; text-transform: uppercase; letter-spacing: 1.5px; color: #555; font-weight: 600; }

/* Engaged E-Brake State */
.ebrake-btn.active { border-color: #ff1744; background: #2a080c; box-shadow: 0 0 16px rgba(255,23,68,0.4); }
.ebrake-btn.active .ebrake-icon { color: #ff1744; border-color: #ff1744; text-shadow: 0 0 8px #ff1744; }
.ebrake-btn.active + .ebrake-label { color: #ff1744; }
</style>
</head>
<body>

  <!-- Sleek Top Telemetry Bar -->
  <div class='header-bar'>
    <div class='brand'>Desky</div>
    <div class='telemetry-group'>
      <!-- Swapped: Status is now on the LEFT side of telemetry -->
      <div class='telemetry-item'>
        <div id='status-dot' class='dot'></div>
        <span id='stat' class='value'>--</span>
      </div>
      <!-- Swapped: DIST is now on the RIGHT side with 3-digit padding -->
      <div class='telemetry-item'>
        <span>DIST</span>
        <span id='dist' class='value'>---</span>
        <span style='font-size: 10px;'>mm</span>
      </div>
      <!-- Reliable SVG Gear Icon -->
      <button class='icon-btn' onclick='toggleSettings()' title='Settings'>
        <svg viewBox="0 0 24 24">
          <path d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/>
        </svg>
      </button>
    </div>
  </div>

  <!-- Hidden Settings Drawer -->
  <div id='drawer' class='drawer'>
    <div class='setting-row'>
      <span>CLIFF LIMIT</span>
      <input type='range' id='threshold' min='30' max='500' value='250' oninput='updateThreshold(this.value)'>
      <span id='thresh-val' class='value'>--</span>
    </div>
    <div class='setting-row'>
      <span>MAX POWER</span>
      <input type='range' id='maxpower' min='10' max='100' value='50' oninput='updateMaxPower(this.value)'>
      <span id='power-val' class='value'>--</span>
    </div>
  </div>

  <!-- Main Drive & Controls Area -->
  <div class='main-stage'>
    
    <!-- Monotone Concentric Canvas Joystick -->
    <div id='joystick-container'>
      <canvas id='joystickCanvas' width='220' height='220'></canvas>
    </div>

    <!-- Automotive E-Brake Button -->
    <div class='ebrake-container'>
      <div id='ebrake-btn' class='ebrake-btn' onclick='toggleEBrake()'>
        <div class='ebrake-icon'>(P)</div>
      </div>
      <div class='ebrake-label'>PARK BRAKE</div>
    </div>

  </div>

<script>
document.addEventListener('gesturestart', function(e) { e.preventDefault(); });
var websocket = new WebSocket('ws://' + window.location.hostname + '/ws');
websocket.binaryType = "arraybuffer";

let lastX = 0, lastY = 0, isHolding = false, heartbeatInterval = null;

const distElem = document.getElementById('dist');
const statElem = document.getElementById('stat');
const statusDot = document.getElementById('status-dot');
const ebrakeBtn = document.getElementById('ebrake-btn');
const threshSlider = document.getElementById('threshold');
const threshVal = document.getElementById('thresh-val');
const powerSlider = document.getElementById('maxpower');
const powerVal = document.getElementById('power-val');

websocket.onmessage = function(event) {
  var data = JSON.parse(event.data);

  if (data.type === 'config') {
    if (document.activeElement !== threshSlider) {
      threshSlider.value = data.threshold;
      threshVal.innerText = data.threshold == 500 ? 'OFF' : data.threshold + 'mm';
    }
    if (data.maxPower !== undefined && document.activeElement !== powerSlider) {
      powerSlider.value = data.maxPower;
      powerVal.innerText = data.maxPower + '%';
    }
  } 
  else if (data.type === 'telemetry') {
    // Zero-pad distance to 3 digits (e.g., 005, 042, 120)
    let distVal = parseInt(data.distance);
    distElem.innerText = isNaN(distVal) ? '---' : String(distVal).padStart(3, '0');

    statElem.innerText = data.status;

    if (data.isCliff || data.isFault) {
      statusDot.className = 'dot dot-danger';
    } else if (data.ebrake) {
      statusDot.className = 'dot dot-warn';
    } else {
      statusDot.className = 'dot dot-ok';
    }

    if (data.ebrake) {
      ebrakeBtn.classList.add('active');
    } else {
      ebrakeBtn.classList.remove('active');
    }
  }
};

function toggleSettings() {
  document.getElementById('drawer').classList.toggle('open');
}

function sendVector(x, y) {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Int8Array([1, x, y]);
    websocket.send(buffer.buffer);
  }
}

function toggleEBrake() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([2]);
    websocket.send(buffer.buffer);
  }
}

function updateThreshold(val) {
  threshVal.innerText = val == 500 ? 'OFF' : val + 'mm';
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([3, (val >> 8) & 0xFF, val & 0xFF]);
    websocket.send(buffer.buffer);
  }
}

function updateMaxPower(val) {
  powerVal.innerText = val + '%';
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([4, val & 0xFF]);
    websocket.send(buffer.buffer);
  }
}

function startHeartbeat() {
  if (!heartbeatInterval) {
    heartbeatInterval = setInterval(function() {
      if (isHolding) sendVector(lastX, lastY);
    }, 100);
  }
}

function stopHeartbeat() {
  if (heartbeatInterval) { clearInterval(heartbeatInterval); heartbeatInterval = null; }
}

const canvas = document.getElementById('joystickCanvas');
const ctx = canvas.getContext('2d');

const center = { x: canvas.width / 2, y: canvas.height / 2 };
const outerRadius = 85;
const innerRadius = 38;
let knobPos = { x: center.x, y: center.y };

function drawJoystick() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  ctx.beginPath();
  ctx.arc(center.x, center.y, outerRadius, 0, Math.PI * 2);
  ctx.fillStyle = '#181818';
  ctx.fill();
  ctx.lineWidth = 1.5;
  ctx.strokeStyle = '#2a2a2a';
  ctx.stroke();

  ctx.beginPath();
  ctx.arc(center.x, center.y, outerRadius - 10, 0, Math.PI * 2);
  ctx.lineWidth = 1;
  ctx.strokeStyle = '#222222';
  ctx.stroke();

  ctx.beginPath();
  ctx.arc(knobPos.x, knobPos.y, innerRadius, 0, Math.PI * 2);
  ctx.fillStyle = isHolding ? '#00adb5' : '#222222';
  ctx.fill();
  ctx.lineWidth = 2;
  ctx.strokeStyle = isHolding ? '#00fff5' : '#333333';
  ctx.stroke();
}

function handlePointer(clientX, clientY) {
  const rect = canvas.getBoundingClientRect();
  const dx = clientX - (rect.left + center.x);
  const dy = clientY - (rect.top + center.y);
  
  const dist = Math.sqrt(dx * dx + dy * dy);
  const maxDist = outerRadius - innerRadius;

  let angle = Math.atan2(dy, dx);
  let clampedDist = Math.min(dist, maxDist);

  knobPos.x = center.x + Math.cos(angle) * clampedDist;
  knobPos.y = center.y + Math.sin(angle) * clampedDist;

  lastX = Math.round((clampedDist / maxDist) * Math.cos(angle) * 100);
  lastY = Math.round((clampedDist / maxDist) * -Math.sin(angle) * 100);

  drawJoystick();
  sendVector(lastX, lastY);
}

function resetJoystick() {
  isHolding = false;
  knobPos = { x: center.x, y: center.y };
  lastX = 0; lastY = 0;
  drawJoystick();
  stopHeartbeat();
  sendVector(0, 0);
}

canvas.addEventListener('pointerdown', (e) => {
  isHolding = true;
  canvas.setPointerCapture(e.pointerId);
  startHeartbeat();
  handlePointer(e.clientX, e.clientY);
});

canvas.addEventListener('pointermove', (e) => {
  if (isHolding) handlePointer(e.clientX, e.clientY);
});

canvas.addEventListener('pointerup', resetJoystick);
canvas.addEventListener('pointercancel', resetJoystick);

drawJoystick();
</script>
</body>
</html>
)raw";

WebServerManager::WebServerManager(RobotStateStore& stateStore)
    : _server(80), _ws("/ws"), _stateStore(stateStore) {}

void WebServerManager::begin() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(250); }

    using namespace std::placeholders;
    _ws.onEvent(std::bind(&WebServerManager::onEvent, this, _1, _2, _3, _4, _5, _6));
    _server.addHandler(&_ws);

    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", HTML_CONTENT);
    });

    _server.begin();
}

void WebServerManager::cleanupClients() {
    _ws.cleanupClients();
}

void WebServerManager::sendConfig(AsyncWebSocketClient *client) {
    ControlState state = _stateStore.getState();
    String json = "{\"type\":\"config\",\"threshold\":" + String(state.cliffThresholdMM) +
                  ",\"maxPower\":" + String(state.maxPowerPercent) + "}";
    if (client) {
        client->text(json);
    } else {
        _ws.textAll(json);
    }
}

void WebServerManager::handleBinaryMessage(void *arg, uint8_t *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_BINARY) {
        uint8_t cmd = data[0];
        if (cmd == 0x01 && len >= 3) {
            _stateStore.updateDriveCommand((int8_t)data[1], (int8_t)data[2]);
        } else if (cmd == 0x02) {
            _stateStore.toggleEBrake();
        } else if (cmd == 0x03 && len >= 3) {
            uint16_t newThreshold = (data[1] << 8) | data[2];
            _stateStore.setCliffThreshold(newThreshold);
            sendConfig();
        } else if (cmd == 0x04 && len >= 2) {
            _stateStore.setMaxPower(data[1]);
            sendConfig();
        }
    }
}

void WebServerManager::onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        sendConfig(client);
    } else if (type == WS_EVT_DATA) {
        handleBinaryMessage(arg, data, len);
    }
}

void WebServerManager::pushTelemetry() {
    ControlState state = _stateStore.getState();
    String json = "{\"type\":\"telemetry\""
                  ",\"distance\":" + String(state.currentDistanceMM) + 
                  ",\"isCliff\":" + String(state.isCliff ? "true" : "false") + 
                  ",\"isFault\":" + String(state.isFault ? "true" : "false") + 
                  ",\"ebrake\":" + String(state.isEBrake ? "true" : "false") + 
                  ",\"status\":\"" + state.status + "\"}";
    _ws.textAll(json);
}

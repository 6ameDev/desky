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
.link-banner {
  display: none; align-items: center; gap: 8px; font-size: 12px; font-weight: 700;
  letter-spacing: 1px; color: #ff5252; cursor: pointer; text-transform: uppercase;
}
.telemetry-group.offline .link-banner { display: flex; }
.telemetry-group.offline .telemetry-item { display: none; }
.controls-dead { opacity: 0.35; pointer-events: none; filter: grayscale(1); }

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
.drawer.open { max-height: 480px; padding: 12px 16px; overflow-y: auto; }
.setting-row { display: flex; align-items: center; gap: 12px; width: 100%; max-width: 360px; font-size: 13px; color: #aaa; }
.setting-row input { flex: 1; accent-color: #00adb5; }
.setting-row button {
  flex: 1; background: #222; color: #e0e0e0; border: 1px solid #444; border-radius: 6px;
  padding: 6px 0; font-size: 12px; font-weight: 700; letter-spacing: 1px; cursor: pointer;
}

/* Main Stage */
.main-stage { flex: 1; width: 100%; display: flex; flex-direction: column; justify-content: space-evenly; align-items: center; }

/* Joystick Canvas */
#joystick-container { display: flex; justify-content: center; align-items: center; width: 220px; height: 220px; }
canvas { display: block; }

/* Automotive E-Brake Button */
.pedal-row { display: flex; gap: 28px; align-items: flex-start; margin-bottom: 10px; }
.ebrake-container { display: flex; flex-direction: column; align-items: center; gap: 6px; }
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

/* Wiggle Button (teal accent, same shape as e-brake) */
.ebrake-btn.wiggle-accent { border-color: #00adb5; }
.ebrake-btn.wiggle-accent .ebrake-icon { color: #00adb5; border-color: #00adb5; }
.ebrake-btn.wiggle-accent:active { background: #06282a; box-shadow: 0 0 16px rgba(0,173,181,0.4); }

/* Mood / Action controls (temporary dev) */
.mood-row { display: flex; gap: 8px; flex-wrap: wrap; justify-content: center; margin: 6px 0 4px; }
.pill { background: #1e1e1e; color: #aaa; border: 1px solid #444; border-radius: 16px; padding: 6px 10px; font-size: 10px; font-weight: 700; letter-spacing: 1px; cursor: pointer; }
.pill.active { color: #fff; border-color: #00adb5; background: #0a2a2c; }
.action-row { display: flex; gap: 8px; flex-wrap: wrap; justify-content: center; margin: 2px 0 8px; }
.pill.play { border-color: #666; }

/* IMU / Orientation Panel */
.imu-panel {
  width: 220px; background: #1a1a1a; border: 1px solid #2a2a2a; border-radius: 10px;
  padding: 10px 12px; display: flex; flex-direction: column; gap: 8px;
}
.imu-title { font-size: 10px; font-weight: 700; letter-spacing: 1.5px; color: #888; display: flex; justify-content: space-between; align-items: center; }
.imu-chip { font-size: 9px; font-weight: 700; letter-spacing: 1px; padding: 3px 8px; border-radius: 10px; background: #222; color: #666; }
.imu-chip.stable { background: #06281a; color: #00e676; }
.imu-chip.alert { background: #2a080c; color: #ff5252; }
.imu-body { display: flex; gap: 12px; align-items: center; }
.horizon {
  width: 76px; height: 76px; border-radius: 50%; overflow: hidden; position: relative; flex-shrink: 0;
  background: #0d2b45; border: 2px solid #333;
}
.horizon-plane { position: absolute; left: -25%; top: -25%; width: 150%; height: 150%; background: linear-gradient(to bottom, #0d2b45 49%, #00adb5 49%, #00adb5 51%, #5a3a1a 51%); }
.imu-readouts { flex: 1; display: flex; flex-direction: column; gap: 4px; }
.imu-row { display: flex; justify-content: space-between; align-items: center; font-size: 11px; color: #aaa; }
</style>
</head>
<body>

  <!-- Sleek Top Telemetry Bar -->
  <div class='header-bar'>
    <div class='brand'>Desky</div>
    <div class='telemetry-group' id='telemetry-group'>
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
      <div id='link-banner' class='link-banner' onclick='retryNow()' title='Tap to retry now'>
        <div class='dot dot-danger'></div>
        <span>OFFLINE &mdash; Connecting (Retry)</span>
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
      <input type='range' id='threshold' min='30' max='500' step='10' value='250' oninput='updateThreshold(this.value)'>
      <span id='thresh-val' class='value'>--</span>
    </div>
    <div class='setting-row'>
      <span>MAX POWER</span>
      <input type='range' id='maxpower' min='10' max='100' step='5' value='50' oninput='updateMaxPower(this.value)'>
      <span id='power-val' class='value'>--</span>
    </div>
    <div class='setting-row'>
      <span>TOF SOFT</span>
      <button id='tof-reset' onclick='resetTof()'>RESET</button>
    </div>
    <div class='setting-row'>
      <span>TOF HARD</span>
      <button id='tof-reset-hard' onclick='resetTofHard()'>RESET</button>
    </div>
    <div class='setting-row'>
      <span>IMU AXIS</span>
      <button id='axis-btn' onclick='cycleAxis()'>AXIS 0</button>
    </div>
    <div class='setting-row'>
      <span>IMU FLAT</span>
      <button onclick='calibrateIMU()'>CALIBRATE</button>
    </div>
    <div class='setting-row'>
      <span>OLED DEBUG</span>
      <button id='oled-dbg-btn' onclick='toggleOledDebug()'>OFF</button>
    </div>
    <div class='setting-row'>
      <span>MPU SENSOR</span>
      <button id='mpu-btn' onclick='toggleMpu()'>ON</button>
    </div>
    <div class='setting-row'>
      <span>GENTLE G</span>
      <input type='range' id='shake-gentle-g' min='10' max='25' step='1' value='13' oninput='updateShakeGentleG(this.value)'>
      <span id='shake-gentle-g-val' class='value'>1.35g</span>
    </div>
    <div class='setting-row'>
      <span>GENTLE GYRO</span>
      <input type='range' id='shake-gentle-gyro' min='1' max='150' step='1' value='50' oninput='updateShakeGentleGyro(this.value)'>
      <span id='shake-gentle-gyro-val' class='value'>50 dps</span>
    </div>
    <div class='setting-row'>
      <span>ANGRY G</span>
      <input type='range' id='shake-angry-g' min='10' max='35' step='1' value='24' oninput='updateShakeAngryG(this.value)'>
      <span id='shake-angry-g-val' class='value'>2.4g</span>
    </div>
    <div class='setting-row'>
      <span>ANGRY GYRO</span>
      <input type='range' id='shake-angry-gyro' min='1' max='350' step='1' value='180' oninput='updateShakeAngryGyro(this.value)'>
      <span id='shake-angry-gyro-val' class='value'>180 dps</span>
    </div>
  </div>

  <!-- Main Drive & Controls Area -->
  <div class='main-stage' id='controls'>
    
    <!-- Monotone Concentric Canvas Joystick -->
    <div id='joystick-container'>
      <canvas id='joystickCanvas' width='220' height='220'></canvas>
    </div>

    <!-- IMU / Orientation Panel -->
    <div class='imu-panel'>
      <div class='imu-title'><span>IMU / ORIENTATION</span><span id='imu-chip' class='imu-chip'>STABLE</span></div>
      <div class='imu-body'>
        <div class='horizon'><div id='horizon-plane' class='horizon-plane'></div></div>
        <div class='imu-readouts'>
          <div class='imu-row'><span>PITCH</span><span id='imu-pitch' class='value'>--</span></div>
          <div class='imu-row'><span>ROLL</span><span id='imu-roll' class='value'>--</span></div>
          <div class='imu-row'><span>GYRO Z</span><span id='imu-gyro' class='value'>--</span></div>
        </div>
      </div>
    </div>

    <!-- Automotive E-Brake + Wiggle Buttons -->
    <div class='pedal-row'>
      <div class='ebrake-container'>
        <div id='ebrake-btn' class='ebrake-btn' onclick='toggleEBrake()'>
          <div class='ebrake-icon'>(P)</div>
        </div>
        <div class='ebrake-label'>PARK BRAKE</div>
      </div>
      <div class='ebrake-container'>
        <div id='wiggle-btn' class='ebrake-btn wiggle-accent' onclick='triggerWiggle()'>
          <div class='ebrake-icon'>~</div>
        </div>
        <div class='ebrake-label'>WIGGLE</div>
      </div>
    </div>

    <!-- Mood + Action (temporary dev) -->
    <div class='mood-row' id='mood-row'>
      <button class='pill' data-mood='0' onclick='setMood(0)'>DEFAULT</button>
      <button class='pill' data-mood='1' onclick='setMood(1)'>TIRED</button>
      <button class='pill' data-mood='2' onclick='setMood(2)'>ANGRY</button>
      <button class='pill' data-mood='3' onclick='setMood(3)'>HAPPY</button>
      <button class='pill' data-mood='4' onclick='setMood(4)'>FOCUSED</button>
      <button class='pill' data-mood='5' onclick='setMood(5)'>SLEEPING</button>
    </div>
    <div class='action-row'>
      <button class='pill play' onclick='playAnim(1)'>BLINK</button>
      <button class='pill play' onclick='playAnim(2)'>CONFUSED</button>
      <button class='pill play' onclick='playAnim(3)'>LAUGH</button>
    </div>

  </div>

<script>
document.addEventListener('gesturestart', function(e) { e.preventDefault(); });
var wsUrl = 'ws://' + window.location.hostname + '/ws';
var websocket = null;
var linkOnline = false;
var reconnectDelay = 1000;
var lastMsgMs = Date.now();
var linkGen = 0;

let lastX = 0, lastY = 0, isHolding = false, heartbeatInterval = null;

const distElem = document.getElementById('dist');
const statElem = document.getElementById('stat');
const statusDot = document.getElementById('status-dot');
const ebrakeBtn = document.getElementById('ebrake-btn');
const threshSlider = document.getElementById('threshold');
const threshVal = document.getElementById('thresh-val');
const powerSlider = document.getElementById('maxpower');
const powerVal = document.getElementById('power-val');
const axisBtn = document.getElementById('axis-btn');
const oledDbgBtn = document.getElementById('oled-dbg-btn');
const mpuBtn = document.getElementById('mpu-btn');
const moodPills = document.querySelectorAll('.mood-row .pill');
const shakeGentleGSlider = document.getElementById('shake-gentle-g');
const shakeGentleGVal = document.getElementById('shake-gentle-g-val');
const shakeGentleGyroSlider = document.getElementById('shake-gentle-gyro');
const shakeGentleGyroVal = document.getElementById('shake-gentle-gyro-val');
const shakeAngryGSlider = document.getElementById('shake-angry-g');
const shakeAngryGVal = document.getElementById('shake-angry-g-val');
const shakeAngryGyroSlider = document.getElementById('shake-angry-gyro');
const shakeAngryGyroVal = document.getElementById('shake-angry-gyro-val');
const telemetryGroup = document.getElementById('telemetry-group');
const controls = document.getElementById('controls');
const imuPitch = document.getElementById('imu-pitch');
const imuRoll = document.getElementById('imu-roll');
const imuGyro = document.getElementById('imu-gyro');
const imuChip = document.getElementById('imu-chip');
const horizonPlane = document.getElementById('horizon-plane');

function setLink(online) {
  linkOnline = online;
  telemetryGroup.classList.toggle('offline', !online);
  controls.classList.toggle('controls-dead', !online);
  if (!online) resetJoystick();
}

function connect() {
  if (websocket && (websocket.readyState === WebSocket.OPEN || websocket.readyState === WebSocket.CONNECTING)) return;
  linkGen++;
  let myGen = linkGen;
  websocket = new WebSocket(wsUrl);
  websocket.binaryType = "arraybuffer";
  let sock = websocket;
  websocket.onopen = function() {
    if (myGen !== linkGen) { try { sock.close(); } catch (e) {} return; }
    lastMsgMs = Date.now();
    reconnectDelay = 1000;
    setLink(true);
  };
  websocket.onmessage = function(event) {
    if (myGen !== linkGen) return;
    handleMessage(event);
  };
  websocket.onclose = function() {
    if (myGen !== linkGen) return;
    declareDead();
  };
  websocket.onerror = function() { try { sock.close(); } catch (e) {} };
}

function declareDead() {
  setLink(false);
  try { websocket.close(); } catch (e) {}
  scheduleReconnect();
}

function scheduleReconnect() {
  let wait = reconnectDelay + Math.random() * 500;
  reconnectDelay = Math.min(reconnectDelay * 2, 5000);
  setTimeout(function() { if (!linkOnline) connect(); }, wait);
}

function retryNow() {
  reconnectDelay = 1000;
  if (websocket && (websocket.readyState === WebSocket.CONNECTING || websocket.readyState === WebSocket.CLOSING)) {
    try { websocket.onopen = null; websocket.onclose = null; websocket.close(); } catch (e) {}
    websocket = null;
  }
  if (!linkOnline) {
    connect();
  } else if (websocket && websocket.readyState === WebSocket.OPEN) {
    setLink(true);
  }
}

function handleMessage(event) {
  lastMsgMs = Date.now();
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
    if (data.imuOrient !== undefined) {
      axisBtn.innerText = 'AXIS ' + data.imuOrient;
    }
    if (data.oledDebug !== undefined) {
      oledDbgBtn.innerText = data.oledDebug ? 'ON' : 'OFF';
    }
    if (data.mpuEnabled !== undefined) {
      mpuBtn.innerText = data.mpuEnabled ? 'ON' : 'OFF';
    }
    if (data.moodOverride !== undefined) {
      moodPills.forEach(function(p){
        var m = parseInt(p.getAttribute('data-mood'));
        p.classList.toggle('active', m === data.moodOverride);
      });
    }
    if (data.shakeGentleG !== undefined && document.activeElement !== shakeGentleGSlider) {
      shakeGentleGSlider.value = Math.round(data.shakeGentleG * 10);
      shakeGentleGVal.innerText = data.shakeGentleG.toFixed(2) + 'g';
    }
    if (data.shakeGentleGyro !== undefined && document.activeElement !== shakeGentleGyroSlider) {
      shakeGentleGyroSlider.value = data.shakeGentleGyro;
      shakeGentleGyroVal.innerText = data.shakeGentleGyro + ' dps';
    }
    if (data.shakeAngryG !== undefined && document.activeElement !== shakeAngryGSlider) {
      shakeAngryGSlider.value = Math.round(data.shakeAngryG * 10);
      shakeAngryGVal.innerText = data.shakeAngryG.toFixed(2) + 'g';
    }
    if (data.shakeAngryGyro !== undefined && document.activeElement !== shakeAngryGyroSlider) {
      shakeAngryGyroSlider.value = data.shakeAngryGyro;
      shakeAngryGyroVal.innerText = data.shakeAngryGyro + ' dps';
    }
  } 
  else if (data.type === 'telemetry') {
    let distVal = parseInt(data.distance);
    let newDist = isNaN(distVal) ? '---' : String(distVal).padStart(3, '0');
    if (distElem.innerText !== newDist) distElem.innerText = newDist;

    if (statElem.innerText !== data.status) statElem.innerText = data.status;

    let newDot = 'dot dot-ok';
    if (data.isCliff || data.isFault || data.tofFault) {
      newDot = 'dot dot-danger';
    } else if (data.ebrake) {
      newDot = 'dot dot-warn';
    }
    if (statusDot.className !== newDot) statusDot.className = newDot;

    ebrakeBtn.classList.toggle('active', !!data.ebrake);

    if (data.mpu !== undefined) {
      let m = data.mpu;
      let pText = m.healthy ? m.pitch.toFixed(1) + '°' : '--';
      let rText = m.healthy ? m.roll.toFixed(1) + '°' : '--';
      let gText = m.healthy ? m.gyroZ.toFixed(1) + '°/s' : '--';
      if (imuPitch.innerText !== pText) imuPitch.innerText = pText;
      if (imuRoll.innerText !== rText) imuRoll.innerText = rText;
      if (imuGyro.innerText !== gText) imuGyro.innerText = gText;

      let chipText = 'STABLE', chipClass = 'imu-chip stable';
      if (!m.healthy) {
        chipText = 'IMU OFFLINE'; chipClass = 'imu-chip';
      }
      if (imuChip.innerText !== chipText) imuChip.innerText = chipText;
      if (imuChip.className !== chipClass) imuChip.className = chipClass;

      if (m.healthy) {
        horizonPlane.style.transform = 'rotate(' + (-m.roll) + 'deg) translateY(' + (-m.pitch * 0.6) + 'px)';
      }
    }
  }
}

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

function triggerWiggle() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([10, 2, 2]);
    websocket.send(buffer.buffer);
  }
}

function resetTof() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([5]);
    websocket.send(buffer.buffer);
  }
}

function resetTofHard() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([6]);
    websocket.send(buffer.buffer);
  }
}

function cycleAxis() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([8]);
    websocket.send(buffer.buffer);
  }
}

function setMood(m) {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([14, m & 0xFF]);
    websocket.send(buffer.buffer);
  }
}

function playAnim(a) {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([15, a & 0xFF]);
    websocket.send(buffer.buffer);
  }
}

function calibrateIMU() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([9]);
    websocket.send(buffer.buffer);
  }
}

function toggleOledDebug() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([12]);
    websocket.send(buffer.buffer);
  }
}

function toggleMpu() {
  if (websocket.readyState === WebSocket.OPEN) {
    let buffer = new Uint8Array([13]);
    websocket.send(buffer.buffer);
  }
}

function updateShakeGentleG(val) {
  var g = (val / 10).toFixed(2);
  shakeGentleGVal.innerText = g + 'g';
  if (websocket.readyState === WebSocket.OPEN) {
    let v = parseInt(val);
    let buffer = new Uint8Array([17, (v >> 8) & 0xFF, v & 0xFF]);
    websocket.send(buffer.buffer);
  }
}
function updateShakeGentleGyro(val) {
  shakeGentleGyroVal.innerText = val + ' dps';
  if (websocket.readyState === WebSocket.OPEN) {
    let v = parseInt(val);
    let buffer = new Uint8Array([18, (v >> 8) & 0xFF, v & 0xFF]);
    websocket.send(buffer.buffer);
  }
}
function updateShakeAngryG(val) {
  var g = (val / 10).toFixed(2);
  shakeAngryGVal.innerText = g + 'g';
  if (websocket.readyState === WebSocket.OPEN) {
    let v = parseInt(val);
    let buffer = new Uint8Array([19, (v >> 8) & 0xFF, v & 0xFF]);
    websocket.send(buffer.buffer);
  }
}
function updateShakeAngryGyro(val) {
  shakeAngryGyroVal.innerText = val + ' dps';
  if (websocket.readyState === WebSocket.OPEN) {
    let v = parseInt(val);
    let buffer = new Uint8Array([20, (v >> 8) & 0xFF, v & 0xFF]);
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
  angle = Math.round(angle / (Math.PI / 4)) * (Math.PI / 4);
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
  if (!linkOnline) return;
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

setInterval(function() {
  if (linkOnline && Date.now() - lastMsgMs > 10000) declareDead();
}, 1000);

drawJoystick();
connect();
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
                  ",\"maxPower\":" + String(state.maxPowerPercent) +
                  ",\"imuOrient\":" + String(state.imuOrientation) +
                  ",\"oledDebug\":" + String(state.displayDebugOn ? "true" : "false") +
                  ",\"mpuEnabled\":" + String(state.mpuEnabled ? "true" : "false") +
                  ",\"moodOverride\":" + String(state.displayMoodOverride) +
                  ",\"shakeGentleG\":" + String(state.shakeGentleG, 2) +
                  ",\"shakeGentleGyro\":" + String((int)state.shakeGentleGyro) +
                  ",\"shakeAngryG\":" + String(state.shakeAngryG, 2) +
                  ",\"shakeAngryGyro\":" + String((int)state.shakeAngryGyro) + "}";
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
        } else if (cmd == 0x05) {            _stateStore.requestTofRecovery(1);
        } else if (cmd == 0x06) {
            _stateStore.requestTofRecovery(2);
        } else if (cmd == 0x08) {
            _stateStore.cycleImuOrientation();
            sendConfig();
        } else if (cmd == 0x09) {
            _stateStore.requestImuCalibrate();
        } else if (cmd == 0x0A && len >= 3) {
            _stateStore.requestWiggle(data[1], data[2]);
        } else if (cmd == 0x0C) {
            _stateStore.toggleDisplayDebug();
            sendConfig();
        } else if (cmd == 0x0D) {
            _stateStore.toggleMpuEnabled();
            sendConfig();
        } else if (cmd == 0x0E && len >= 2) {
            _stateStore.setDisplayMood(data[1]);
            sendConfig();
        } else if (cmd == 0x0F && len >= 2) {
            _stateStore.requestDisplayAnim(data[1]);
        } else if (cmd == 0x11 && len >= 3) {
            int v = (data[1] << 8) | data[2];
            _stateStore.setShakeGentle(v / 10.0f, _stateStore.getState().shakeGentleGyro);
            sendConfig();
        } else if (cmd == 0x12 && len >= 3) {
            int v = (data[1] << 8) | data[2];
            _stateStore.setShakeGentle(_stateStore.getState().shakeGentleG, v);
            sendConfig();
        } else if (cmd == 0x13 && len >= 3) {
            int v = (data[1] << 8) | data[2];
            _stateStore.setShakeAngry(v / 10.0f, _stateStore.getState().shakeAngryGyro);
            sendConfig();
        } else if (cmd == 0x14 && len >= 3) {
            int v = (data[1] << 8) | data[2];
            _stateStore.setShakeAngry(_stateStore.getState().shakeAngryG, v);
            sendConfig();
        }
        // Any WebUI direct command takes precedence over interpreted petting (500ms window) and wakes
        // Mood (0x0E) and anim (0x0F) are direct but must not clear forced mood via wakeFromSleep
        bool isDirect = (cmd == 0x01 || cmd == 0x02 || cmd == 0x03 || cmd == 0x04 || cmd == 0x05 || cmd == 0x06 || cmd == 0x08 || cmd == 0x09 || cmd == 0x0A || cmd == 0x0C || cmd == 0x0D || cmd == 0x0E || cmd == 0x0F || cmd == 0x11 || cmd == 0x12 || cmd == 0x13 || cmd == 0x14);
        bool shouldWake = (cmd == 0x01 || cmd == 0x02 || cmd == 0x03 || cmd == 0x04 || cmd == 0x05 || cmd == 0x06 || cmd == 0x08 || cmd == 0x09 || cmd == 0x0A || cmd == 0x0C || cmd == 0x0D || cmd == 0x11 || cmd == 0x12 || cmd == 0x13 || cmd == 0x14);
        if (isDirect) {
            _stateStore.markDirectCommand();
        }
        if (shouldWake) {
            _stateStore.wakeFromSleep();
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
                  ",\"tofFault\":" + String(state.tofFault ? "true" : "false") + 
                  ",\"status\":\"" + state.status + "\"" +
                  ",\"mpu\":{\"pitch\":" + String(state.imu.pitch, 1) +
                  ",\"roll\":" + String(state.imu.roll, 1) +
                  ",\"gyroZ\":" + String(state.imu.gyroZ, 1) +
                  ",\"accelMag\":" + String(state.imu.accelMag, 2) +
                  ",\"healthy\":" + String(state.imu.healthy ? "true" : "false") + "}}";
    _ws.textAll(json);
    _lastPushMs = millis();
    _lastSentCliff = state.isCliff;
    _lastSentFault = state.isFault;
    _lastSentEBrake = state.isEBrake;
    _lastSentStatus = state.status;
    _lastSentDistance = state.currentDistanceMM;
    _lastSentImuHealthy = state.imu.healthy;
}

void WebServerManager::pushTelemetryIfNeeded() {
    ControlState state = _stateStore.getState();
    bool eventChanged = (state.isCliff != _lastSentCliff) ||
                        (state.isFault != _lastSentFault) ||
                        (state.isEBrake != _lastSentEBrake) ||
                        (state.imu.healthy != _lastSentImuHealthy) ||
                        (state.status != _lastSentStatus);
    bool distanceMoved = abs(state.currentDistanceMM - _lastSentDistance) > TELEMETRY_DISTANCE_EPSILON_MM;
    unsigned long interval = (eventChanged || distanceMoved) ? TELEMETRY_INTERVAL_MS : TELEMETRY_IDLE_INTERVAL_MS;
    if (!eventChanged && !distanceMoved && (millis() - _lastPushMs < interval)) {
        return;
    }
    pushTelemetry();
}

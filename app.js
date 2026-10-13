const SERVICE_UUID = '7d7a0001-4b52-4b52-8a6e-4d494e495052';
const TELEMETRY_UUID = '7d7a0002-4b52-4b52-8a6e-4d494e495052';

const connectBtn = document.getElementById('connectBtn');
const demoBtn = document.getElementById('demoBtn');
const statusDot = document.getElementById('statusDot');
const connectionText = document.getElementById('connectionText');
const lastUpdateEl = document.getElementById('lastUpdate');
const packetCountEl = document.getElementById('packetCount');
const rawPacketEl = document.getElementById('rawPacket');
const mapBtn = document.getElementById('mapBtn');

let bluetoothDevice = null;
let telemetryCharacteristic = null;
let rxBuffer = '';
let packetCount = 0;
let notificationCount = 0;
let latestLat = null;
let latestLon = null;
let demoTimer = null;
let telemetryWatchdog = null;
let lastNotificationAt = 0;

const decoder = new TextDecoder();
const history = Array(70).fill(1);
const canvas = document.getElementById('accelChart');
const ctx = canvas.getContext('2d');

function setConnection(connected, text) {
  statusDot.classList.toggle('online', connected);
  statusDot.classList.toggle('offline', !connected);
  connectionText.textContent = text;
  connectBtn.textContent = connected ? 'Connected' : 'Connect Base ESP32';
}

function parsePacket(packet) {
  const result = {};
  packet.split(',').forEach(part => {
    const index = part.indexOf('=');
    if (index > 0) {
      result[part.slice(0, index).trim()] = part.slice(index + 1).trim();
    }
  });
  return result;
}

function numeric(value, fallback = null) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function text(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function fmt(value, digits = 1, suffix = '') {
  const n = numeric(value);
  return n === null ? `—${suffix}` : `${n.toFixed(digits)}${suffix}`;
}

function linkQuality(rssi, snr) {
  if (rssi === null) return '—';
  if (rssi > -60 && (snr === null || snr > 5)) return 'Excellent';
  if (rssi > -80) return 'Good';
  if (rssi > -100) return 'Fair';
  return 'Weak';
}

function updateDashboard(packet) {
  packet = packet.trim();

  // A valid trekker packet always starts with AX= and contains several fields.
  if (!packet.includes('AX=') || !packet.includes('AM=')) return;

  const d = parsePacket(packet);
  const fall = d.F === '1';
  const gpsFix = d.GPS === '1';
  const fingerDetected = d.FINGER !== undefined ? d.FINGER === '1' : numeric(d.IR, 0) >= 5000;
  const hr = numeric(d.HR, 0);
  const accel = numeric(d.AM, 0);
  const rssi = numeric(d.RSSI);
  const snr = numeric(d.SNR);

  packetCount += 1;
  packetCountEl.textContent = packetCount;
  lastUpdateEl.textContent = new Date().toLocaleTimeString();
  rawPacketEl.textContent = packet;

  text('heartRate', hr > 0 ? Math.round(hr) : '—');
  text('fingerState', fingerDetected ? 'Detected' : 'No signal');
  text('accelMag', `${accel.toFixed(2)} g`);

  text('temperature', fmt(d.T, 1, ' °C'));
  text('pressure', fmt(d.P, 1, ' hPa'));
  text('altitude', fmt(d.ALT, 1, ' m'));

  text('ax', fmt(d.AX, 2));
  text('ay', fmt(d.AY, 2));
  text('az', fmt(d.AZ, 2));
  text('gx', fmt(d.GX, 1));
  text('gy', fmt(d.GY, 1));
  text('gz', fmt(d.GZ, 1));

  text('rssi', rssi === null ? '—' : Math.round(rssi));
  text('snr', snr === null ? '— dB' : `${snr.toFixed(1)} dB`);
  text('linkQuality', linkQuality(rssi, snr));

  const fallBanner = document.getElementById('fallBanner');
  const safetyPill = document.getElementById('safetyPill');
  fallBanner.classList.toggle('hidden', !fall);
  safetyPill.textContent = fall ? 'FALL DETECTED' : 'NORMAL';
  safetyPill.className = `pill ${fall ? 'danger' : 'good'}`;

  const gpsPill = document.getElementById('gpsPill');
  if (gpsFix) {
    latestLat = numeric(d.LAT);
    latestLon = numeric(d.LON);
    gpsPill.textContent = 'FIXED';
    gpsPill.className = 'pill good';
    text('coordinates', `${numeric(d.LAT, 0).toFixed(6)}, ${numeric(d.LON, 0).toFixed(6)}`);
    text('satellites', d.SAT ?? '0');
    mapBtn.disabled = latestLat === null || latestLon === null;
  } else {
    latestLat = null;
    latestLon = null;
    gpsPill.textContent = 'NO FIX';
    gpsPill.className = 'pill warn';
    text('coordinates', 'Waiting for GPS fix');
    text('satellites', d.SAT ?? '0');
    mapBtn.disabled = true;
  }

  history.push(accel || 0);
  if (history.length > 70) history.shift();
  drawChart();
}

function drawChart() {
  const width = canvas.width;
  const height = canvas.height;
  ctx.clearRect(0, 0, width, height);

  ctx.strokeStyle = 'rgba(255,255,255,0.08)';
  ctx.lineWidth = 1;
  [0.25, 0.5, 0.75].forEach(p => {
    ctx.beginPath();
    ctx.moveTo(0, height * p);
    ctx.lineTo(width, height * p);
    ctx.stroke();
  });

  const maxValue = Math.max(3.2, ...history);
  ctx.strokeStyle = '#4bd4ff';
  ctx.lineWidth = 3;
  ctx.beginPath();

  history.forEach((value, i) => {
    const x = (i / Math.max(1, history.length - 1)) * width;
    const y = height - Math.min(1, value / maxValue) * (height - 12) - 6;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });

  ctx.stroke();
}

function consumeBleText(chunk) {
  notificationCount += 1;
  lastNotificationAt = Date.now();

  // Every packet begins with AX=. If a new packet starts before we saw the
  // previous newline, discard the damaged partial packet and resynchronise.
  const packetStart = chunk.indexOf('AX=');
  if (packetStart >= 0 && rxBuffer.length > 0) {
    rxBuffer = chunk.slice(packetStart);
  } else {
    rxBuffer += chunk;
  }

  // Avoid an indefinitely growing buffer if a BLE notification is lost.
  if (rxBuffer.length > 1000) {
    const latestStart = rxBuffer.lastIndexOf('AX=');
    rxBuffer = latestStart >= 0 ? rxBuffer.slice(latestStart) : '';
  }

  let newlineIndex;
  while ((newlineIndex = rxBuffer.indexOf('\n')) >= 0) {
    const line = rxBuffer.slice(0, newlineIndex).trim();
    rxBuffer = rxBuffer.slice(newlineIndex + 1);
    if (line) updateDashboard(line);
  }
}

function onNotification(event) {
  const chunk = decoder.decode(event.target.value);
  consumeBleText(chunk);
}

function startTelemetryWatchdog() {
  clearInterval(telemetryWatchdog);
  telemetryWatchdog = setInterval(() => {
    if (!bluetoothDevice?.gatt?.connected) return;

    if (lastNotificationAt === 0) {
      setConnection(true, 'BLE connected — waiting for telemetry');
      return;
    }

    const age = Date.now() - lastNotificationAt;
    if (age > 4000) {
      setConnection(true, 'BLE connected — telemetry stalled');
    } else {
      setConnection(true, `Live BLE telemetry · ${notificationCount} notifications`);
    }
  }, 1000);
}

async function connectBluetooth() {
  if (!navigator.bluetooth) {
    alert('Web Bluetooth is not supported in this browser. Use Chrome/Edge on Windows/macOS/Linux or Chrome on Android.');
    return;
  }

  if (demoTimer) {
    clearInterval(demoTimer);
    demoTimer = null;
  }

  try {
    connectBtn.disabled = true;
    connectBtn.textContent = 'Connecting…';
    rxBuffer = '';
    notificationCount = 0;
    lastNotificationAt = 0;

    bluetoothDevice = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: 'Trekker-Base' }],
      optionalServices: [SERVICE_UUID]
    });

    bluetoothDevice.addEventListener('gattserverdisconnected', () => {
      clearInterval(telemetryWatchdog);
      rxBuffer = '';
      setConnection(false, 'Disconnected');
      connectBtn.disabled = false;
    });

    const server = await bluetoothDevice.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    telemetryCharacteristic = await service.getCharacteristic(TELEMETRY_UUID);

    telemetryCharacteristic.addEventListener('characteristicvaluechanged', onNotification);
    await telemetryCharacteristic.startNotifications();

    // Verify that the characteristic itself is readable. The base initially
    // exposes BASE_READY; telemetry then arrives through notifications.
    try {
      const initialValue = await telemetryCharacteristic.readValue();
      const initialText = decoder.decode(initialValue).trim();
      console.log('Initial BLE value:', initialText);
    } catch (readError) {
      console.warn('Initial characteristic read failed, notifications may still work:', readError);
    }

    setConnection(true, 'BLE connected — waiting for telemetry');
    startTelemetryWatchdog();
  } catch (error) {
    console.error(error);
    setConnection(false, `BLE failed: ${error.message || error}`);
  } finally {
    connectBtn.disabled = false;
  }
}

function startDemo() {
  if (demoTimer) {
    clearInterval(demoTimer);
    demoTimer = null;
    demoBtn.textContent = 'Demo';
    setConnection(false, 'Disconnected');
    return;
  }

  demoBtn.textContent = 'Stop Demo';
  setConnection(true, 'Demo telemetry');

  let phase = 0;
  demoTimer = setInterval(() => {
    phase += 0.25;
    const impact = Math.random() > 0.985;
    const packet = [
      `AX=${(Math.sin(phase) * 0.03).toFixed(2)}`,
      `AY=${(Math.cos(phase) * 0.04).toFixed(2)}`,
      `AZ=${(1.01 + Math.sin(phase * .7) * .02).toFixed(2)}`,
      `AM=${(impact ? 3.18 : 1.01 + Math.random() * .04).toFixed(2)}`,
      `GX=${(-2 + Math.random() * 4).toFixed(1)}`,
      `GY=${(-2 + Math.random() * 4).toFixed(1)}`,
      `GZ=${(-1 + Math.random() * 2).toFixed(1)}`,
      `T=${(29.7 + Math.sin(phase / 10) * .4).toFixed(1)}`,
      `P=${(1013.4 + Math.sin(phase / 7) * .7).toFixed(1)}`,
      `ALT=${(-1.4 + Math.sin(phase / 7) * 4).toFixed(1)}`,
      `IR=68421`,
      `RED=61234`,
      `HR=${Math.round(76 + Math.sin(phase / 3) * 5)}`,
      `FINGER=1`,
      `F=${impact ? 1 : 0}`,
      `GPS=1`,
      `LAT=19.076090`,
      `LON=72.877426`,
      `SAT=8`,
      `RSSI=-42`,
      `SNR=9.5`
    ].join(',');

    updateDashboard(packet);
  }, 500);
}

connectBtn.addEventListener('click', connectBluetooth);
demoBtn.addEventListener('click', startDemo);
mapBtn.addEventListener('click', () => {
  if (latestLat === null || latestLon === null) return;
  window.open(`https://www.openstreetmap.org/?mlat=${latestLat}&mlon=${latestLon}#map=16/${latestLat}/${latestLon}`, '_blank', 'noopener');
});

window.addEventListener('resize', drawChart);
drawChart();

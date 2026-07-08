// Tracking engine: device registry, adaptive poll scheduler, target selection,
// threshold-based change detection, compact WebSocket push frames.

const fs = require('fs');
const path = require('path');
const upstream = require('./upstream');

const DEVICES_FILE = path.join(__dirname, '..', 'data', 'devices.json');

// Poll cadence (ms)
const POLL_ACTIVE_MS = 8000;      // aircraft being tracked
const POLL_IDLE_MS = 30000;       // nothing in range / flight not airborne
const BACKOFF_MAX_MS = 300000;    // cap after repeated failures / 429s
const FLIGHT_LOST_GRACE_MS = 5 * 60 * 1000; // callsign mode: keep waiting 5 min

// Push thresholds — a state frame is only sent when a value moves past these.
const TH_ALT_FT = 100;
const TH_GS_KT = 5;
const TH_TRK_DEG = 2;

// Radius-mode hysteresis: switch target only if challenger is 20% closer.
const SWITCH_RATIO = 0.8;

// ICAO type designator -> on-device bitmap family key (device falls back to GENERIC).
function bitmapFamily(typeCode) {
  const t = (typeCode || '').toUpperCase();
  if (/^B73/.test(t)) return 'B737';
  if (/^B7[4-9]/.test(t)) return 'B747'; // widebody Boeing bucket
  if (/^A3[0-2]/.test(t) || /^A19N|^A20N|^A21N/.test(t)) return 'A320';
  if (/^A3[3-9]/.test(t)) return 'A330';
  if (/^A22/.test(t)) return 'A320';
  if (/^E1[79]|^E29|^CRJ/.test(t)) return 'RJ';
  if (/^AT[47]|^DH8|^SF3/.test(t)) return 'PROP';
  if (/^C1[57-9]|^C2[08]|^P28|^SR2|^DA4/.test(t)) return 'GA';
  return 'GENERIC';
}

// --- Device registry --------------------------------------------------------

// id -> { config: {mode, lat, lon, radiusKm, callsign}, socket, state }
const devices = new Map();

function defaultConfig() {
  // layout: device flight-screen layout (0 = classic, 1 = big image).
  return { mode: 'radius', lat: null, lon: null, radiusKm: 50, callsign: '', layout: 0 };
}

function loadDevices() {
  try {
    const saved = JSON.parse(fs.readFileSync(DEVICES_FILE, 'utf8'));
    for (const [id, config] of Object.entries(saved)) {
      devices.set(id, freshDevice({ ...defaultConfig(), ...config }));
    }
  } catch (_) { /* no saved devices yet */ }
}

function saveDevices() {
  const out = {};
  for (const [id, d] of devices) out[id] = d.config;
  fs.mkdirSync(path.dirname(DEVICES_FILE), { recursive: true });
  fs.writeFileSync(DEVICES_FILE, JSON.stringify(out, null, 2));
}

function freshDevice(config) {
  return {
    config,
    socket: null,
    lastSeen: 0,
    // tracking state
    target: null,        // normalized aircraft currently on the gauges
    lastPushed: null,    // { altFt, gsKt, trackDeg } last sent past thresholds
    flightMeta: null,    // enriched metadata of current flight (for status API)
    lostSince: 0,
    nextPollAt: 0,
    failStreak: 0,
  };
}

function getOrCreateDevice(id) {
  let d = devices.get(id);
  if (!d) {
    d = freshDevice(defaultConfig());
    devices.set(id, d);
    saveDevices();
  }
  return d;
}

function setConfig(id, patch) {
  const d = getOrCreateDevice(id);
  const prev = d.config;
  const c = { ...prev };
  if (patch.mode === 'radius' || patch.mode === 'flight') c.mode = patch.mode;
  if (typeof patch.lat === 'number' && patch.lat >= -90 && patch.lat <= 90) c.lat = patch.lat;
  if (typeof patch.lon === 'number' && patch.lon >= -180 && patch.lon <= 180) c.lon = patch.lon;
  if (typeof patch.radiusKm === 'number') c.radiusKm = Math.min(100, Math.max(1, patch.radiusKm));
  if (typeof patch.callsign === 'string') c.callsign = patch.callsign.trim().toUpperCase().slice(0, 8);
  if (patch.layout === 0 || patch.layout === 1) c.layout = patch.layout;
  d.config = c;

  // A change to WHAT we track needs a tracking reset + fresh poll; a
  // presentation-only change (layout) must NOT drop the current flight.
  const trackingChanged = c.mode !== prev.mode || c.lat !== prev.lat || c.lon !== prev.lon
    || c.radiusKm !== prev.radiusKm || c.callsign !== prev.callsign;
  if (trackingChanged) {
    d.target = null; d.lastPushed = null; d.flightMeta = null; d.lostSince = 0; d.nextPollAt = 0;
  }
  saveDevices();
  sendCfg(d);
  // Redraw the device immediately in the (possibly new) layout without a re-poll.
  if (!trackingChanged && d.target && d.flightMeta) sendFlight(d);
  return c;
}

// "Change network": tell the device to wipe Wi-Fi creds and reboot into its
// setup portal. Returns false if the device isn't currently connected.
function resetNetwork(id) {
  const d = devices.get(id);
  if (!d || !d.socket || d.socket.readyState !== 1) return false;
  send(d, { t: 'netreset' });
  return true;
}

// --- WebSocket plumbing ------------------------------------------------------

function attachSocket(id, socket) {
  const d = getOrCreateDevice(id);
  if (d.socket && d.socket !== socket) { try { d.socket.close(); } catch (_) {} }
  d.socket = socket;
  d.lastSeen = Date.now();
  d.nextPollAt = 0; // poll promptly for a fresh connection
  sendCfg(d);
  // Replay current flight so a reconnecting device redraws immediately.
  if (d.target && d.flightMeta) sendFlight(d);
  else send(d, { t: 'clear' });
}

function detachSocket(id, socket) {
  const d = devices.get(id);
  if (d && d.socket === socket) d.socket = null;
}

function send(d, obj) {
  if (d.socket && d.socket.readyState === 1) {
    try { d.socket.send(JSON.stringify(obj)); } catch (_) {}
  }
}

function sendCfg(d) {
  send(d, { t: 'cfg', mode: d.config.mode, radiusKm: d.config.radiusKm,
            cs: d.config.callsign, layout: d.config.layout });
}

function sendFlight(d) {
  const m = d.flightMeta, a = d.target;
  send(d, {
    t: 'flight',
    cs: a.callsign || m.registration || a.hex.toUpperCase(),
    al: m.airline || 'UNKNOWN AIRLINE',
    rt: m.route || '--- -> ---',
    ac: m.acName || (a.typeCode || 'UNKNOWN'),
    fam: m.fam,
    alt: quant(a.altFt, TH_ALT_FT),
    gs: a.gsKt,
    trk: a.trackDeg,
  });
  d.lastPushed = { altFt: a.altFt, gsKt: a.gsKt, trackDeg: a.trackDeg };
}

function quant(v, q) { return Math.round(v / q) * q; }

// --- Poll scheduler ----------------------------------------------------------

// Dedupe: devices with the same rounded area share one upstream call per tick.
async function tick() {
  const now = Date.now();
  const due = [...devices.entries()].filter(([, d]) =>
    d.socket && d.socket.readyState === 1 && now >= d.nextPollAt && !d.polling && isConfigured(d));
  if (due.length === 0) return;

  const areaResults = new Map(); // areaKey -> Promise<aircraft[]>
  await Promise.all(due.map(async ([id, d]) => {
    d.polling = true;
    try {
      await pollDevice(d, areaResults);
      d.failStreak = 0;
    } catch (err) {
      d.failStreak++;
      const backoff = Math.min(BACKOFF_MAX_MS, POLL_ACTIVE_MS * 2 ** d.failStreak);
      d.nextPollAt = Date.now() + backoff;
      console.error(`[poll] device ${id} failed (streak ${d.failStreak}, retry in ${backoff / 1000}s):`, err.message);
    } finally {
      d.polling = false;
    }
  }));
}

function isConfigured(d) {
  const c = d.config;
  if (c.mode === 'radius') return c.lat != null && c.lon != null;
  return !!c.callsign;
}

async function pollDevice(d, areaResults) {
  const c = d.config;
  let candidates;
  if (c.mode === 'radius') {
    const key = `${c.lat.toFixed(3)},${c.lon.toFixed(3)},${c.radiusKm}`;
    if (!areaResults.has(key)) areaResults.set(key, upstream.aircraftNearPoint(c.lat, c.lon, c.radiusKm));
    candidates = (await areaResults.get(key)).filter((a) => !a.onGround);
    // adsb.lol radius is in whole NM (>= requested); enforce the exact km radius.
    candidates = candidates.filter((a) => haversineKm(c.lat, c.lon, a.lat, a.lon) <= c.radiusKm);
  } else {
    const key = `cs:${c.callsign}`;
    if (!areaResults.has(key)) areaResults.set(key, upstream.aircraftByCallsign(c.callsign));
    candidates = (await areaResults.get(key)).filter((a) => !a.onGround);
  }

  const target = pickTarget(d, candidates);
  if (!target) {
    handleNoTarget(d);
    return;
  }

  d.lostSince = 0;
  const isNewFlight = !d.target || d.target.hex !== target.hex;
  d.target = target;

  if (isNewFlight) {
    d.flightMeta = await enrich(target);
    sendFlight(d);
  } else if (movedPastThreshold(d.lastPushed, target)) {
    send(d, { t: 'state', alt: quant(target.altFt, TH_ALT_FT), gs: target.gsKt, trk: target.trackDeg });
    d.lastPushed = { altFt: target.altFt, gsKt: target.gsKt, trackDeg: target.trackDeg };
  }
  d.nextPollAt = Date.now() + POLL_ACTIVE_MS;
}

function pickTarget(d, candidates) {
  if (candidates.length === 0) return null;
  if (d.config.mode === 'flight') return candidates[0];
  const { lat, lon } = d.config;
  const withDist = candidates
    .map((a) => ({ a, dist: haversineKm(lat, lon, a.lat, a.lon) }))
    .sort((x, y) => x.dist - y.dist);
  const closest = withDist[0];
  if (d.target) {
    const cur = withDist.find((x) => x.a.hex === d.target.hex);
    // Keep current target unless it left / challenger is meaningfully closer.
    if (cur && closest.dist >= cur.dist * SWITCH_RATIO) return cur.a;
  }
  return closest.a;
}

function handleNoTarget(d) {
  if (d.target) {
    if (d.config.mode === 'flight') {
      // Grace period: the flight may be between legs or out of coverage.
      if (!d.lostSince) d.lostSince = Date.now();
      if (Date.now() - d.lostSince < FLIGHT_LOST_GRACE_MS) {
        d.nextPollAt = Date.now() + POLL_ACTIVE_MS;
        return;
      }
    }
    d.target = null; d.lastPushed = null; d.flightMeta = null; d.lostSince = 0;
    send(d, { t: 'clear' });
  }
  d.nextPollAt = Date.now() + POLL_IDLE_MS;
}

function movedPastThreshold(last, a) {
  if (!last) return true;
  return Math.abs(a.altFt - last.altFt) >= TH_ALT_FT
    || Math.abs(a.gsKt - last.gsKt) >= TH_GS_KT
    || angleDiff(a.trackDeg, last.trackDeg) >= TH_TRK_DEG;
}

function angleDiff(a, b) {
  const d = Math.abs(a - b) % 360;
  return d > 180 ? 360 - d : d;
}

async function enrich(a) {
  const [info, route] = await Promise.all([
    upstream.lookupAircraft(a.hex),
    upstream.lookupRoute(a.callsign),
  ]);
  const typeCode = (info && info.typeCode) || a.typeCode;
  return {
    airline: (route && route.airline) || '',
    route: route && route.origin && route.destination
      ? `${route.origin.iata} -> ${route.destination.iata}` : '',
    routeNames: route && route.origin && route.destination
      ? { from: route.origin.name, to: route.destination.name } : null,
    acName: info ? [info.manufacturer, info.type].filter(Boolean).join(' ') : '',
    registration: (info && info.registration) || '',
    fam: bitmapFamily(typeCode),
  };
}

function haversineKm(lat1, lon1, lat2, lon2) {
  const R = 6371, toRad = (x) => (x * Math.PI) / 180;
  const dLat = toRad(lat2 - lat1), dLon = toRad(lon2 - lon1);
  const s = Math.sin(dLat / 2) ** 2
    + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(s));
}

function status(id) {
  const d = devices.get(id);
  if (!d) return null;
  return {
    id,
    online: !!(d.socket && d.socket.readyState === 1),
    config: d.config,
    flight: d.target ? {
      callsign: d.target.callsign,
      hex: d.target.hex,
      altFt: d.target.altFt,
      gsKt: d.target.gsKt,
      trackDeg: d.target.trackDeg,
      lat: d.target.lat,
      lon: d.target.lon,
      ...d.flightMeta,
    } : null,
  };
}

function listDevices() {
  return [...devices.keys()].map((id) => status(id));
}

function start() {
  loadDevices();
  setInterval(() => tick().catch((e) => console.error('[tick]', e)), 1000);
}

module.exports = { start, attachSocket, detachSocket, setConfig, resetNetwork, getOrCreateDevice, status, listDevices };

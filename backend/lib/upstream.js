// Upstream flight-data clients + persistent cache.
//
// Live positions : adsb.lol  (free, no key; radius in nautical miles)
// Route/aircraft : adsbdb.com (free, no key; static data -> cached hard)
// Photos         : planespotters.net pub API (free; descriptive UA required)
//
// Cache policy: aircraft-by-hex forever, callsign routes 24 h, photo URLs 7 d,
// negative results 6 h.

const fs = require('fs');
const path = require('path');

const ADSB_BASE = process.env.ADSB_BASE || 'https://api.adsb.lol/v2';
const ADSBDB_BASE = process.env.ADSBDB_BASE || 'https://api.adsbdb.com/v0';
const PS_BASE = process.env.PLANESPOTTERS_BASE || 'https://api.planespotters.net/pub/photos';

// planespotters rejects generic UAs; it wants an app name + contact.
const USER_AGENT = 'flight-tracker-gauge/1.0 (+mailto:odiljanandith@gmail.com)';

const CACHE_FILE = path.join(__dirname, '..', 'data', 'cache.json');
const ROUTE_TTL_MS = 24 * 3600 * 1000;
const PHOTO_TTL_MS = 7 * 24 * 3600 * 1000;
const NEGATIVE_TTL_MS = 6 * 3600 * 1000;

const KM_PER_NM = 1.852;

let cache = { aircraft: {}, routes: {}, photos: {} };
try {
  cache = JSON.parse(fs.readFileSync(CACHE_FILE, 'utf8'));
  cache.aircraft = cache.aircraft || {};
  cache.routes = cache.routes || {};
  cache.photos = cache.photos || {};
} catch (_) { /* fresh cache */ }

let saveTimer = null;
function scheduleSave() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    fs.mkdirSync(path.dirname(CACHE_FILE), { recursive: true });
    fs.writeFile(CACHE_FILE, JSON.stringify(cache), (err) => {
      if (err) console.error('[cache] save failed:', err.message);
    });
  }, 5000);
}

async function getJson(url, timeoutMs = 10000) {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(url, {
      signal: ctrl.signal,
      headers: { 'accept': 'application/json', 'user-agent': USER_AGENT },
    });
    if (res.status === 429) { const e = new Error('rate limited'); e.rateLimited = true; throw e; }
    if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
    return await res.json();
  } finally {
    clearTimeout(t);
  }
}

// --- Live positions -------------------------------------------------------

// Aircraft within radiusKm of (lat, lon). Returns normalized list.
async function aircraftNearPoint(lat, lon, radiusKm) {
  const radiusNm = Math.min(250, Math.ceil(radiusKm / KM_PER_NM));
  const data = await getJson(`${ADSB_BASE}/point/${lat.toFixed(4)}/${lon.toFixed(4)}/${radiusNm}`);
  return (data.ac || []).map(normalizeAc).filter(Boolean);
}

// Live state for one callsign (may match several airframes; caller picks).
async function aircraftByCallsign(callsign) {
  const cs = callsign.trim().toUpperCase();
  const data = await getJson(`${ADSB_BASE}/callsign/${encodeURIComponent(cs)}`);
  return (data.ac || []).map(normalizeAc).filter(Boolean);
}

function normalizeAc(ac) {
  if (!ac || ac.lat == null || ac.lon == null) return null;
  const baro = typeof ac.alt_baro === 'number' ? ac.alt_baro : null; // "ground" when landed
  return {
    hex: (ac.hex || '').trim().toLowerCase(),
    callsign: (ac.flight || '').trim().toUpperCase(),
    lat: ac.lat,
    lon: ac.lon,
    altFt: baro != null ? baro : (typeof ac.alt_geom === 'number' ? ac.alt_geom : 0),
    onGround: ac.alt_baro === 'ground',
    gsKt: typeof ac.gs === 'number' ? Math.round(ac.gs) : 0,
    trackDeg: typeof ac.track === 'number' ? Math.round(ac.track) : 0,
    typeCode: (ac.t || '').trim().toUpperCase(), // ICAO type designator, e.g. B738
  };
}

// --- Static enrichment (adsbdb, cached) ------------------------------------

// { type, typeCode, registration } or null. Cached forever (positive) / 6 h (negative).
async function lookupAircraft(hex) {
  if (!hex) return null;
  const hit = cache.aircraft[hex];
  if (hit && (hit.v || Date.now() - hit.ts < NEGATIVE_TTL_MS)) return hit.v || null;
  let v = null;
  try {
    const data = await getJson(`${ADSBDB_BASE}/aircraft/${encodeURIComponent(hex)}`);
    const a = data && data.response && data.response.aircraft;
    if (a) {
      v = {
        type: a.type || '',
        typeCode: (a.icao_type || '').toUpperCase(),
        manufacturer: a.manufacturer || '',
        registration: a.registration || '',
      };
    }
  } catch (err) {
    if (!String(err.message).includes('HTTP 404')) console.error('[adsbdb aircraft]', hex, err.message);
  }
  cache.aircraft[hex] = { v, ts: Date.now() };
  scheduleSave();
  return v;
}

// { origin:{iata,name}, destination:{iata,name}, airline } or null. Cached 24 h.
async function lookupRoute(callsign) {
  if (!callsign) return null;
  const cs = callsign.trim().toUpperCase();
  const hit = cache.routes[cs];
  if (hit && Date.now() - hit.ts < (hit.v ? ROUTE_TTL_MS : NEGATIVE_TTL_MS)) return hit.v || null;
  let v = null;
  try {
    const data = await getJson(`${ADSBDB_BASE}/callsign/${encodeURIComponent(cs)}`);
    const fr = data && data.response && data.response.flightroute;
    if (fr) {
      v = {
        airline: (fr.airline && fr.airline.name) || '',
        origin: fr.origin ? { iata: fr.origin.iata_code || fr.origin.icao_code || '', name: fr.origin.name || '' } : null,
        destination: fr.destination ? { iata: fr.destination.iata_code || fr.destination.icao_code || '', name: fr.destination.name || '' } : null,
      };
    }
  } catch (err) {
    if (!String(err.message).includes('HTTP 404')) console.error('[adsbdb route]', cs, err.message);
  }
  cache.routes[cs] = { v, ts: Date.now() };
  scheduleSave();
  return v;
}

// --- Aircraft photos (planespotters, cached) --------------------------------

// { src, photographer } of the best thumbnail for an airframe, or null.
// URL lookup cached 7 d (positive) / 6 h (negative); transient API errors are
// NOT cached so a hiccup doesn't blank a photo for hours.
async function lookupPhoto(hex) {
  if (!hex) return null;
  const hit = cache.photos[hex];
  if (hit && Date.now() - hit.ts < (hit.v ? PHOTO_TTL_MS : NEGATIVE_TTL_MS)) return hit.v || null;
  let v = null;
  try {
    const data = await getJson(`${PS_BASE}/hex/${encodeURIComponent(hex)}`);
    const p = data && data.photos && data.photos[0];
    if (p && p.thumbnail_large && p.thumbnail_large.src) {
      v = { src: p.thumbnail_large.src, photographer: p.photographer || '' };
    }
  } catch (err) {
    console.error('[planespotters]', hex, err.message);
    return hit ? hit.v || null : null;
  }
  cache.photos[hex] = { v, ts: Date.now() };
  scheduleSave();
  return v;
}

module.exports = { aircraftNearPoint, aircraftByCallsign, lookupAircraft, lookupRoute, lookupPhoto, USER_AGENT, KM_PER_NM };

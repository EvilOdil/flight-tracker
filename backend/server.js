// Flight-tracker backend: REST config API + device WebSocket + mobile web app.
// Run: npm install && npm start   (Node >= 18)

const http = require('http');
const fs = require('fs');
const path = require('path');
const express = require('express');
const sharp = require('sharp');
const { WebSocketServer } = require('ws');
const tracker = require('./lib/tracker');
const upstream = require('./lib/upstream');

const PORT = process.env.PORT || 8080;

// Device photo panel box (Classic layout on the 320x480 TFT); photos are
// resized to fit inside it once and cached on disk, so each airframe costs
// one planespotters lookup + one download ever.
const PHOTO_W = 282;
const PHOTO_H = 217;
const PHOTO_DIR = path.join(__dirname, 'data', 'photos');

// Panel colour compensation: the device's TN glass renders midtone neutrals
// with a pink cast (green response sags mid-range; full-scale colours are
// fine, which is why text looks right). Pre-distort each channel with an
// inverse gamma so greys land grey ON THE PANEL. Endpoints are pinned, so
// whites/blacks are untouched. Tune via env without code changes:
// >1 darkens a channel's midtones, <1 lifts them.
const PANEL_GAMMA_R = parseFloat(process.env.PANEL_GAMMA_R || '1.08');
const PANEL_GAMMA_G = parseFloat(process.env.PANEL_GAMMA_G || '0.88');
const PANEL_GAMMA_B = parseFloat(process.env.PANEL_GAMMA_B || '1.06');
function gammaLut(g) {
  const lut = new Uint8Array(256);
  for (let i = 0; i < 256; i++) lut[i] = Math.round(255 * Math.pow(i / 255, g));
  return lut;
}
const LUT_R = gammaLut(PANEL_GAMMA_R);
const LUT_G = gammaLut(PANEL_GAMMA_G);
const LUT_B = gammaLut(PANEL_GAMMA_B);

const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// --- REST API (used by the mobile web app) ---------------------------------

app.get('/api/devices', (_req, res) => res.json(tracker.listDevices()));

app.get('/api/devices/:id/status', (req, res) => {
  const s = tracker.status(req.params.id.toUpperCase());
  if (!s) return res.status(404).json({ error: 'unknown device' });
  res.json(s);
});

app.get('/api/devices/:id/config', (req, res) => {
  const d = tracker.getOrCreateDevice(req.params.id.toUpperCase());
  res.json(d.config);
});

app.put('/api/devices/:id/config', (req, res) => {
  const cfg = tracker.setConfig(req.params.id.toUpperCase(), req.body || {});
  res.json(cfg);
});

// "Change network": push a reset command so the device reboots into its Wi-Fi
// setup portal. 409 if the device isn't currently connected.
app.post('/api/devices/:id/reset-network', (req, res) => {
  const ok = tracker.resetNetwork(req.params.id.toUpperCase());
  if (!ok) return res.status(409).json({ error: 'device offline' });
  res.json({ ok: true });
});

// Panel calibration pattern (PHOTO_CAL=1): served INSTEAD of every aircraft
// photo so the device draws it through the exact photo pipeline. Photograph
// the screen, compare with the known pattern, derive the panel's channel
// response, bake the inverse into the LUTs. Bypasses the compensation LUTs
// on purpose — it must measure the raw panel.
// Bands top->bottom: R ramp, G ramp, B ramp, grey ramp,
//   patches [sky-blue, blue, red, green, orange, 50% grey],
//   patches [white, black, 25% grey, 75% grey].
let calCache = null;
async function calibrationJpg() {
  if (calCache) return calCache;
  const W = PHOTO_W, H = PHOTO_H;
  const data = Buffer.alloc(W * H * 3);
  const bandH = Math.floor(H / 6);
  const hues = [[135, 180, 235], [0, 0, 255], [255, 0, 0], [0, 255, 0], [255, 165, 0], [128, 128, 128]];
  const greys = [[255, 255, 255], [0, 0, 0], [64, 64, 64], [192, 192, 192]];
  for (let y = 0; y < H; y++) {
    const band = Math.min(5, Math.floor(y / bandH));
    for (let x = 0; x < W; x++) {
      const i = (y * W + x) * 3;
      const v = Math.round((255 * x) / (W - 1));
      let c;
      if (band === 0) c = [v, 0, 0];
      else if (band === 1) c = [0, v, 0];
      else if (band === 2) c = [0, 0, v];
      else if (band === 3) c = [v, v, v];
      else if (band === 4) c = hues[Math.min(5, Math.floor((6 * x) / W))];
      else c = greys[Math.min(3, Math.floor((4 * x) / W))];
      data[i] = c[0]; data[i + 1] = c[1]; data[i + 2] = c[2];
    }
  }
  calCache = await sharp(data, { raw: { width: W, height: H, channels: 3 } })
    .jpeg({ quality: 95, progressive: false, chromaSubsampling: '4:4:4' })
    .toBuffer();
  return calCache;
}

// Aircraft photo for the device's Classic layout: planespotters thumbnail,
// resized server-side to the panel box, baseline JPEG (TJpg_Decoder can't do
// progressive), cached on disk by hex. Requested ONLY by devices actually in
// Classic mode, so Standard-mode devices cost zero photo traffic. 404 -> the
// device falls back to its built-in silhouette bitmaps.
app.get('/api/aircraft/:hex/photo', async (req, res) => {
  if (process.env.PHOTO_CAL === '1') {
    res.set('content-type', 'image/jpeg');
    res.set('cache-control', 'no-store');
    res.set('x-photographer', 'CALIBRATION PATTERN');
    return res.send(await calibrationJpg());
  }
  const hex = String(req.params.hex).toLowerCase().replace(/[^0-9a-f]/g, '').slice(0, 6);
  if (hex.length !== 6) return res.status(400).json({ error: 'bad hex' });
  // v3: 4:4:4 chroma (4:2:0 smears hues at this size) + per-channel panel
  // gamma compensation (see LUTs above). Cache key includes the gamma values
  // so retuning via env regenerates stale thumbs automatically.
  const gTag = `${PANEL_GAMMA_R}-${PANEL_GAMMA_G}-${PANEL_GAMMA_B}`;
  const file = path.join(PHOTO_DIR, `${hex}.v3.${gTag}.jpg`);
  const metaFile = path.join(PHOTO_DIR, `${hex}.json`);
  try {
    if (!fs.existsSync(file)) {
      const p = await upstream.lookupPhoto(hex);
      if (!p) return res.status(404).json({ error: 'no photo' });
      const r = await fetch(p.src, { headers: { 'user-agent': upstream.USER_AGENT } });
      if (!r.ok) return res.status(404).json({ error: 'photo fetch failed' });
      const { data, info } = await sharp(Buffer.from(await r.arrayBuffer()))
        .resize(PHOTO_W, PHOTO_H, { fit: 'inside' })
        .raw()
        .toBuffer({ resolveWithObject: true });
      for (let i = 0; i < data.length; i += info.channels) {
        data[i] = LUT_R[data[i]];
        data[i + 1] = LUT_G[data[i + 1]];
        data[i + 2] = LUT_B[data[i + 2]];
      }
      const jpg = await sharp(data, { raw: info })
        .jpeg({ quality: 88, progressive: false, chromaSubsampling: '4:4:4' })
        .toBuffer();
      fs.mkdirSync(PHOTO_DIR, { recursive: true });
      fs.writeFileSync(file, jpg);
      fs.writeFileSync(metaFile, JSON.stringify({ photographer: p.photographer }));
    }
    let credit = '';
    try { credit = JSON.parse(fs.readFileSync(metaFile, 'utf8')).photographer || ''; } catch (_) {}
    res.set('content-type', 'image/jpeg');
    res.set('cache-control', 'public, max-age=604800');
    if (credit) res.set('x-photographer', credit);
    res.send(fs.readFileSync(file));
  } catch (err) {
    console.error('[photo]', hex, err.message);
    res.status(404).json({ error: 'photo error' });
  }
});

// --- Device WebSocket --------------------------------------------------------

const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws/device' });

wss.on('connection', (socket) => {
  let deviceId = null;

  socket.on('message', (buf) => {
    let msg;
    try { msg = JSON.parse(buf.toString()); } catch (_) { return; }
    if (msg.t === 'hello' && typeof msg.id === 'string' && msg.id.length >= 4) {
      deviceId = msg.id.toUpperCase().slice(0, 16);
      tracker.attachSocket(deviceId, socket);
      console.log(`[ws] device ${deviceId} connected (fw ${msg.fw || '?'})`);
    }
  });

  socket.on('close', () => {
    if (deviceId) {
      tracker.detachSocket(deviceId, socket);
      console.log(`[ws] device ${deviceId} disconnected`);
    }
  });
  socket.on('error', () => {});

  // Keepalive: drop dead sockets so polling stops.
  socket.isAlive = true;
  socket.on('pong', () => { socket.isAlive = true; });
});

setInterval(() => {
  for (const socket of wss.clients) {
    if (!socket.isAlive) { socket.terminate(); continue; }
    socket.isAlive = false;
    try { socket.ping(); } catch (_) {}
  }
}, 20000);

tracker.start();
server.listen(PORT, () => console.log(`flight-tracker backend on http://0.0.0.0:${PORT}`));

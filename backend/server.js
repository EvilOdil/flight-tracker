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

// NOTE (2026-07-09): this file used to carry per-pixel tone/green
// compensation LUTs for the clone panel's apparent "folded" brightness
// response. That whole saga turned out to be a TFT driver mismatch: with
// ILI9488_DRIVER selected in TFT_eSPI's User_Setup.h (was ILI9486_DRIVER)
// the panel renders photos correctly with NO server-side correction —
// confirmed on the physical glass. The LUT implementation (tone gamma +
// ceiling + piecewise green curve, all env-tunable) lives in git history
// should a future panel batch need it back.

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

// Panel test pattern (PHOTO_CAL=1): served INSTEAD of every aircraft photo
// so the device draws it through the exact photo pipeline. Photograph the
// screen and compare with the known pattern to characterize the bare panel
// (this is how the ILI9486-vs-ILI9488 driver mismatch was diagnosed).
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
      data[i] = c[0];
      data[i + 1] = c[1];
      data[i + 2] = c[2];
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
    res.set('x-photographer', 'CAL PATTERN (raw)');
    return res.send(await calibrationJpg());
  }
  const hex = String(req.params.hex).toLowerCase().replace(/[^0-9a-f]/g, '').slice(0, 6);
  if (hex.length !== 6) return res.status(400).json({ error: 'bad hex' });
  // v4: plain fit-inside resize, no colour processing (see driver note at the
  // top). 4:4:4 chroma stays — 4:2:0 smears hues at this size — and baseline
  // stays (TJpg_Decoder can't decode progressive). The version in the cache
  // key is what invalidates stale renders; bump it if this processing ever
  // changes again.
  const file = path.join(PHOTO_DIR, `${hex}.v4.jpg`);
  const metaFile = path.join(PHOTO_DIR, `${hex}.json`);
  try {
    if (!fs.existsSync(file)) {
      const p = await upstream.lookupPhoto(hex);
      if (!p) return res.status(404).json({ error: 'no photo' });
      const r = await fetch(p.src, { headers: { 'user-agent': upstream.USER_AGENT } });
      if (!r.ok) return res.status(404).json({ error: 'photo fetch failed' });
      const jpg = await sharp(Buffer.from(await r.arrayBuffer()))
        .resize(PHOTO_W, PHOTO_H, { fit: 'inside' })
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

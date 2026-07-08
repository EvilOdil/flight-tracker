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

// Panel tone compensation. The clone glass has a NON-MONOTONIC response:
// steep brightening from black up to input ~PANEL_TONE_MAX, a fold (darker!)
// through the upper mid-range, recovering only at full scale — measured with
// the PHOTO_CAL pattern (light grey rendered darker than dark grey; sky blue
// went purple because its green channel sat in the fold). A folded curve is
// not invertible, so instead all photo tones are compressed into the
// monotonic zone: pre-darken with a gamma (the panel's steep low end lifts
// it back) and ceiling at the fold threshold. The SAME curve on every
// channel means hues can no longer twist; the cost is slightly compressed
// highlights. Identity: PANEL_TONE_GAMMA=1 PANEL_TONE_MAX=255.
const PANEL_TONE_GAMMA = parseFloat(process.env.PANEL_TONE_GAMMA || '2.2');
// 148: with 160 the top of the compressed range still brushed the start of
// the fold (orange's red ceiling sagged -> read yellow-green on glass).
const PANEL_TONE_MAX = parseInt(process.env.PANEL_TONE_MAX || '148', 10);
// Per-channel trim on top of the tone curve. The panel over-lifts green in
// its LOW zone (greenish near-black greys, greenish orange), and a gamma
// suppresses lows relatively harder than mids — hence green 1.12.
const PANEL_GAMMA_R = parseFloat(process.env.PANEL_GAMMA_R || '1.0');
const PANEL_GAMMA_G = parseFloat(process.env.PANEL_GAMMA_G || '1.12');
const PANEL_GAMMA_B = parseFloat(process.env.PANEL_GAMMA_B || '1.0');
function channelLut(gamma) {
  const lut = new Uint8Array(256);
  for (let i = 0; i < 256; i++) {
    // SCALE into [0, TONE_MAX] (not clip!): a hard ceiling crushed every
    // bright value to the same level, desaturating bright hues (sky blue
    // came out whitish). Scaling keeps channel ratios across the range.
    const toned = Math.round(PANEL_TONE_MAX * Math.pow(i / 255, PANEL_TONE_GAMMA));
    lut[i] = Math.round(255 * Math.pow(toned / 255, gamma));
  }
  return lut;
}
const LUT_R = channelLut(PANEL_GAMMA_R);
const LUT_G = channelLut(PANEL_GAMMA_G);
const LUT_B = channelLut(PANEL_GAMMA_B);

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
// PHOTO_CAL=1 serves the raw pattern (measures the bare panel);
// PHOTO_CAL=2 serves it THROUGH the tone-compensation LUTs (verifies the
// compensation: grey order must come out correct, sky-blue must read blue).
const calCache = {};
async function calibrationJpg(applyLuts) {
  const key = applyLuts ? 'lut' : 'raw';
  if (calCache[key]) return calCache[key];
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
      data[i] = applyLuts ? LUT_R[c[0]] : c[0];
      data[i + 1] = applyLuts ? LUT_G[c[1]] : c[1];
      data[i + 2] = applyLuts ? LUT_B[c[2]] : c[2];
    }
  }
  calCache[key] = await sharp(data, { raw: { width: W, height: H, channels: 3 } })
    .jpeg({ quality: 95, progressive: false, chromaSubsampling: '4:4:4' })
    .toBuffer();
  return calCache[key];
}

// PHOTO_CAL=3: green-gamma tuning grid. Six columns (white dots at the top:
// 1 dot = leftmost), each rendering the same four rows — dark grey 64,
// light grey 192, orange, sky blue — through the tone pipeline with an
// increasingly strong green trim. The user picks the column whose greys are
// neutral AND whose orange is orange; that gamma becomes the default.
const GRID_G = [1.0, 1.15, 1.3, 1.45, 1.6, 1.75];
let gridCache = null;
async function tuningGridJpg() {
  if (gridCache) return gridCache;
  const W = PHOTO_W, H = PHOTO_H;
  const data = Buffer.alloc(W * H * 3);
  const rows = [[64, 64, 64], [192, 192, 192], [255, 165, 0], [135, 180, 235]];
  const lutRB = channelLut(1.0);
  const lutGs = GRID_G.map((g) => channelLut(g));
  const colW = W / GRID_G.length, rowH = H / (rows.length + 0.5);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const i = (y * W + x) * 3;
      const col = Math.min(GRID_G.length - 1, Math.floor(x / colW));
      // top strip: white dot markers (col+1 dots), raw white for visibility
      if (y < rowH * 0.5) {
        const inCol = x - col * colW;
        const dot = Math.floor(inCol / 12);
        const isDot = dot < col + 1 && (inCol % 12) < 8 && y > 4 && y < 16;
        const v = isDot ? 255 : 0;
        data[i] = v; data[i + 1] = v; data[i + 2] = v;
        continue;
      }
      const row = Math.min(rows.length - 1, Math.floor((y - rowH * 0.5) / rowH));
      const c = rows[row];
      data[i] = lutRB[c[0]];
      data[i + 1] = lutGs[col][c[1]];
      data[i + 2] = lutRB[c[2]];
    }
  }
  gridCache = await sharp(data, { raw: { width: W, height: H, channels: 3 } })
    .jpeg({ quality: 95, progressive: false, chromaSubsampling: '4:4:4' })
    .toBuffer();
  return gridCache;
}

// Aircraft photo for the device's Classic layout: planespotters thumbnail,
// resized server-side to the panel box, baseline JPEG (TJpg_Decoder can't do
// progressive), cached on disk by hex. Requested ONLY by devices actually in
// Classic mode, so Standard-mode devices cost zero photo traffic. 404 -> the
// device falls back to its built-in silhouette bitmaps.
app.get('/api/aircraft/:hex/photo', async (req, res) => {
  const cal = process.env.PHOTO_CAL;
  if (cal === '1' || cal === '2' || cal === '3') {
    res.set('content-type', 'image/jpeg');
    res.set('cache-control', 'no-store');
    res.set('x-photographer', cal === '3' ? 'TUNING GRID (pick a column)'
      : cal === '2' ? 'CAL PATTERN (compensated)' : 'CAL PATTERN (raw)');
    return res.send(cal === '3' ? await tuningGridJpg() : await calibrationJpg(cal === '2'));
  }
  const hex = String(req.params.hex).toLowerCase().replace(/[^0-9a-f]/g, '').slice(0, 6);
  if (hex.length !== 6) return res.status(400).json({ error: 'bad hex' });
  // v3: 4:4:4 chroma (4:2:0 smears hues at this size) + panel tone
  // compensation (see LUTs above). Cache key includes all tuning values so a
  // retune via env regenerates stale thumbs automatically.
  const gTag = `${PANEL_TONE_GAMMA}-${PANEL_TONE_MAX}-${PANEL_GAMMA_R}-${PANEL_GAMMA_G}-${PANEL_GAMMA_B}`;
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

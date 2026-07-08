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

// Aircraft photo for the device's Classic layout: planespotters thumbnail,
// resized server-side to the panel box, baseline JPEG (TJpg_Decoder can't do
// progressive), cached on disk by hex. Requested ONLY by devices actually in
// Classic mode, so Standard-mode devices cost zero photo traffic. 404 -> the
// device falls back to its built-in silhouette bitmaps.
app.get('/api/aircraft/:hex/photo', async (req, res) => {
  const hex = String(req.params.hex).toLowerCase().replace(/[^0-9a-f]/g, '').slice(0, 6);
  if (hex.length !== 6) return res.status(400).json({ error: 'bad hex' });
  const file = path.join(PHOTO_DIR, `${hex}.jpg`);
  const metaFile = path.join(PHOTO_DIR, `${hex}.json`);
  try {
    if (!fs.existsSync(file)) {
      const p = await upstream.lookupPhoto(hex);
      if (!p) return res.status(404).json({ error: 'no photo' });
      const r = await fetch(p.src, { headers: { 'user-agent': upstream.USER_AGENT } });
      if (!r.ok) return res.status(404).json({ error: 'photo fetch failed' });
      const jpg = await sharp(Buffer.from(await r.arrayBuffer()))
        .resize(PHOTO_W, PHOTO_H, { fit: 'inside' })
        .jpeg({ quality: 78, progressive: false })
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

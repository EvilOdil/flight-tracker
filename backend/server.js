// Flight-tracker backend: REST config API + device WebSocket + mobile web app.
// Run: npm install && npm start   (Node >= 18)

const http = require('http');
const path = require('path');
const express = require('express');
const { WebSocketServer } = require('ws');
const tracker = require('./lib/tracker');

const PORT = process.env.PORT || 8080;

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

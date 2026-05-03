const express = require('express');
const path = require('path');
const { spawn } = require('child_process');
const http = require('http');
const WebSocket = require('ws');

const app = express();
const PORT = process.env.PORT || 3000;
const RTSP_WS_PORT = process.env.RTSP_WS_PORT || 9999;

const RTSP_URL = process.env.RTSP_URL || 'rtsp://100.68.81.224:9554/ds-test';

app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json());

app.post('/emit', (req, res) => {
  const extraArgs = req.body?.id ? [`--id=${req.body.id}`] : [];
  const child = spawn('node', [path.join(__dirname, 'emit-target.js'), '1', ...extraArgs], { cwd: __dirname });
  child.stdout.on('data', (d) => process.stdout.write(d));
  child.stderr.on('data', (d) => process.stderr.write(d));
  child.on('close', (code) => {
    res.json({ ok: code === 0 });
  });
});

app.listen(PORT, () => {
  console.log(`Client server running at http://localhost:${PORT}`);
});

// ---------------------------------------------------------------------------
// RTSP → MPEG1 WebSocket relay
// ---------------------------------------------------------------------------

const rtspServer = http.createServer();
const wss = new WebSocket.Server({ server: rtspServer });

let ffmpeg = null;
let clientCount = 0;

function startFfmpeg() {
  if (ffmpeg) return;

  console.log(`[rtsp] Starting ffmpeg for ${RTSP_URL}`);
  ffmpeg = spawn('ffmpeg', [
    '-rtsp_transport', 'tcp',
    '-timeout', '5000000',
    '-i', RTSP_URL,
    '-f', 'mpegts',
    '-codec:v', 'mpeg1video',
    '-b:v', '1000k',
    '-r', '25',
    '-vf', 'scale=640:360',
    '-bf', '0',
    'pipe:1',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  ffmpeg.stderr.on('data', (d) => {
    if (process.env.RTSP_DEBUG) process.stderr.write(d);
  });

  ffmpeg.stdout.on('data', (data) => {
    wss.clients.forEach((client) => {
      if (client.readyState === WebSocket.OPEN) {
        try { client.send(data); } catch (_) {}
      }
    });
  });

  ffmpeg.on('close', (code) => {
    console.log(`[rtsp] ffmpeg exited (${code})`);
    ffmpeg = null;
    if (clientCount > 0) {
      console.log('[rtsp] Restarting ffmpeg in 2s...');
      setTimeout(startFfmpeg, 2000);
    }
  });
}

function stopFfmpeg() {
  if (ffmpeg) {
    console.log('[rtsp] Stopping ffmpeg');
    ffmpeg.kill('SIGTERM');
    ffmpeg = null;
  }
}

wss.on('connection', (ws) => {
  clientCount++;
  console.log(`[rtsp] Client connected (${clientCount} total)`);
  if (clientCount === 1) startFfmpeg();

  ws.on('close', () => {
    clientCount--;
    console.log(`[rtsp] Client disconnected (${clientCount} remaining)`);
    if (clientCount === 0) stopFfmpeg();
  });
});

rtspServer.listen(RTSP_WS_PORT, () => {
  console.log(`RTSP relay WebSocket listening on ws://localhost:${RTSP_WS_PORT}`);
});
